// AVM2 字节码解释器 + Flash 显示对象模型实现。
#include "swf/Fvm.hpp"

#include "swf/Avm2Opcodes.hpp"   // opcode 编号 / 操作数 / 栈效应单一事实来源

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <set>

#include "swf/Decoder.hpp"   // Movie 完整定义
#include "swf/Render.hpp"    // hitTestRuntimeStage（运行时显示树命中测试）

#include <unordered_map>
#include <functional>

namespace swf {
namespace fvm {

// 前置声明（定义见文件后部“鼠标输入”一节）：舞台像素 -> 对象局部像素坐标。
static bool displayLocalPoint(const std::shared_ptr<DisplayObject>& d, const Movie& movie,
                              double sx, double sy, double& lx, double& ly);

// 是否为 SWF 按钮角色（DefineButton2）：按钮没有时间轴，交互态由鼠标命中链维护后交给渲染器。
static bool isButtonDisplay(const Movie& movie, const std::shared_ptr<DisplayObject>& d) {
    if (!d || !d->characterId) return false;
    auto it = movie.characters.find(d->characterId);
    return it != movie.characters.end() && it->second &&
           it->second->kind == Character::Kind::Button;
}

// anc 是否为 d 自身或其祖先（用于判断按下目标是否仍在该按钮内）。
static bool containsDisplay(const std::shared_ptr<DisplayObject>& anc,
                            const std::shared_ptr<DisplayObject>& d) {
    for (auto c = d; c; c = c->parent)
        if (c == anc) return true;
    return false;
}

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

// ---------- M6: AVM2 opcode Command 化 ----------
// 执行上下文：承载 exec 局部状态，供 opcode handler 操作（仿 M5 的 Frame）。
// AS3 throw 用 C++ 异常在递归的 exec() 之间传播，直到某一层找到匹配的
// exception_info（catch 块）；最外层仍未捕获时按 Flash 记录并中止该脚本。
struct AsThrow {
    Value value;
};
static bool exceptionTypeMatches(const Value& ex, const std::string& want);

struct ExecCtx {
    std::vector<Value>& stack;
    std::vector<Value>& locals;
    std::vector<std::shared_ptr<Object>>& scope;
    Inst& in;
    const abc::MethodBody& body;
    std::shared_ptr<Object> thisObj;
    Vm& vm;
    size_t codeSize = 0;
    bool done = false;
    Value returnValue;
    void push(const Value& v) { stack.push_back(v); }
    Value pop() { if (stack.empty()) return Value::undefined(); Value v = stack.back(); stack.pop_back(); return v; }
    void jump(int32_t off) {
        size_t t = static_cast<size_t>(static_cast<int64_t>(in.pos()) + off);
        if (t <= codeSize) in.seek(t);
    }
    // 按 exception_info 的 [from,to,target,excType,varName] 查找处理器。
    // excType=0 为 catch-all；否则按类型名（含短名与继承链）匹配。
    int findHandler(size_t pc, const Value& ex) const {
        const auto& eh = body.exceptionHandlers;
        for (size_t k = 0; k + 4 < eh.size(); k += 5) {
            if (pc < eh[k] || pc >= eh[k + 1]) continue;
            const uint32_t typeIdx = eh[k + 3];
            if (typeIdx == 0) return static_cast<int>(k);
            const std::string want = vm.multinameName(typeIdx);
            if (want.empty() || exceptionTypeMatches(ex, want))
                return static_cast<int>(k);
        }
        return -1;
    }
};
using FvmOp = std::function<void(ExecCtx&)>;
class FvmOpRegistry {
    std::unordered_map<int, FvmOp> m_;
public:
    void add(int op, FvmOp h) { m_[op] = std::move(h); }
    FvmOp* find(int op) { auto it = m_.find(op); return it == m_.end() ? nullptr : &it->second; }
    bool has(int op) const { return m_.find(op) != m_.end(); }
    std::vector<int> codes() const {
        std::vector<int> out;
        out.reserve(m_.size());
        for (const auto& kv : m_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }
};
inline FvmOpRegistry& fvmOps() { static FvmOpRegistry r; return r; }
static std::vector<std::string> enumerableKeys(const std::shared_ptr<Object>& o);  // 前向声明（定义见文件后部）
static bool looseEquals(const Value& a, const Value& b);
static bool strictEquals(const Value& a, const Value& b);
static bool valueIsType(const Value& v, const std::shared_ptr<Object>& t);
static bool declaresProperty(const std::shared_ptr<Object>& o, const std::string& name);
// 运行时 multiname：RTQName 只压 ns，RTQNameL/MultinameL 压 name(+ns)。
static int mnRuntimeStackPops(const abc::AbcFile& abc, uint32_t index);
static bool mnHasRuntimeNameValue(const abc::AbcFile& abc, uint32_t index);
static std::shared_ptr<Object> findTypeObject(Vm& vm, const std::string& name);
static std::string xmlEscapeString(const std::string& s, bool attribute);
static Value dmRead(Vm& vm, std::uint32_t addr, int kind);   // domainMemory 小端读
static void dmWrite(Vm& vm, std::uint32_t addr, int kind, double val);
// E4X：递归收集指定名字的后代节点（getdescendants / XML.descendants 共用）。
static void xmlCollectDescendants(const std::shared_ptr<Object>& node,
                                  const std::string& want,
                                  std::vector<Value>& out, int depth);

// 从栈上弹出运行时 name/ns，返回最终属性名。
// 调用点必须在「参数已弹出、receiver 尚未弹出」的位置。
static std::string takeRuntimeName(ExecCtx& c, uint32_t mn) {
    const auto& A = c.vm.abcFile();
    std::string name = c.vm.multinameName(mn);
    const int pops = mnRuntimeStackPops(A, mn);
    const bool nameOnStack = mnHasRuntimeNameValue(A, mn);
    for (int k = 0; k < pops; ++k) {
        Value v = c.pop();
        if (k == 0 && nameOnStack) name = v.toString();
    }
    return name;
}

inline void registerFvmOps() {
    // 第一批：纯栈 / 常量 push（无副作用，低风险验证基础设施）
    fvmOps().add(0x02, [](ExecCtx&) {});                                              // nop
    fvmOps().add(0x09, [](ExecCtx&) {});                                              // label
    fvmOps().add(0x20, [](ExecCtx& c) { c.stack.push_back(Value::nullv()); });         // push null
    fvmOps().add(0x21, [](ExecCtx& c) { c.stack.push_back(Value::undefined()); });     // push undefined
    fvmOps().add(0x26, [](ExecCtx& c) { c.stack.push_back(Value::boolean(true)); });    // push true
    fvmOps().add(0x27, [](ExecCtx& c) { c.stack.push_back(Value::boolean(false)); });   // push false
    fvmOps().add(0x28, [](ExecCtx& c) { c.stack.push_back(Value::num(NAN)); });         // push nan
    fvmOps().add(0x29, [](ExecCtx& c) { if (!c.stack.empty()) c.stack.pop_back(); });   // pop
    fvmOps().add(0x2a, [](ExecCtx& c) { if (!c.stack.empty()) c.stack.push_back(c.stack.back()); }); // dup
    fvmOps().add(0x24, [](ExecCtx& c) { c.stack.push_back(Value::intg((int8_t)c.in.u8())); }); // push byte

    // ---- 第二批：作用域/枚举/属性/调用（不涉及控制流跳转）----
    fvmOps().add(0x1c, [](ExecCtx& c) {  // pushwith
        Value v = c.pop(); if (v.kind == VKind::Object && v.obj) c.scope.push_back(v.obj); });
    fvmOps().add(0x04, [](ExecCtx& c) {  // getsuper: obj,[ns],[name] -> value（跳过本类覆盖）
        uint32_t mn = c.in.u30();
        std::string name = takeRuntimeName(c, mn);
        Value obj = c.pop();
        if (obj.kind != VKind::Object || !obj.obj) { c.push(Value::undefined()); return; }
        Value result = Value::undefined();
        for (auto cd = obj.obj->cls ? obj.obj->cls->superClass : nullptr; cd; cd = cd->superClass) {
            bool declared = false;
            for (const auto& m : cd->instanceMembers) if (m.name == name) { declared = true; break; }
            if (!declared) continue;
            auto sp = obj.obj->slots.find(name);
            if (sp != obj.obj->slots.end()) result = sp->second;
            else { auto pp = obj.obj->props.find(name); if (pp != obj.obj->props.end()) result = pp->second; }
            break;
        }
        if (result.isUndefined()) result = c.vm.getProperty(obj.obj, name);
        c.push(result); });
    fvmOps().add(0x05, [](ExecCtx& c) {  // setsuper: obj,[ns],[name],value -> (空)
        uint32_t mn = c.in.u30();
        Value val = c.pop();
        std::string name = takeRuntimeName(c, mn);
        Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) c.vm.setProperty(obj.obj, name, val); });
    fvmOps().add(0x59, [](ExecCtx& c) {  // getdescendants: obj,[ns],[name] -> XMLList
        uint32_t mn = c.in.u30();
        const std::string want = takeRuntimeName(c, mn);
        Value obj = c.pop();
        auto list = c.vm.newObject(ObjKind::Array, "XMLList");
        std::vector<Value> found;
        if (obj.kind == VKind::Object && obj.obj)
            xmlCollectDescendants(obj.obj, want, found, 0);
        for (std::size_t k = 0; k < found.size(); ++k)
            list->props[std::to_string(k)] = found[k];
        list->props["$length"] = Value::intg(static_cast<int32_t>(found.size()));
        c.push(Value::object(list)); });
    fvmOps().add(0x6a, [](ExecCtx& c) {  // deleteproperty: obj,[ns],[name] -> bool
        uint32_t mn = c.in.u30();
        std::string name = takeRuntimeName(c, mn);
        Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) { obj.obj->props.erase(name); c.push(Value::boolean(true)); }
        else c.push(Value::boolean(false)); });
    fvmOps().add(0x57, [](ExecCtx& c) {  // newactivation
        auto o = c.vm.newObject(ObjKind::Generic, "Activation");
        for (const auto& t : c.body.traits) {
            if (t.kind != 0x00 && t.kind != 0x06) continue;
            std::string nm = c.vm.multinameName(t.name); if (nm.empty()) continue;
            o->slots[nm] = Value::undefined();
            o->props["$slot_" + std::to_string(t.slotId)] = Value::str(nm);
        }
        c.push(Value::object(o)); });
    fvmOps().add(0x5a, [](ExecCtx& c) {  // newcatch
        uint32_t idx = c.in.u30(); auto o = c.vm.newObject(ObjKind::Generic, "Catch");
        o->props["$catchIdx"] = Value::uintv(idx); c.push(Value::object(o)); });
    fvmOps().add(0x45, [](ExecCtx& c) {  // callsuper
        uint32_t mn = c.in.u30(); uint32_t argc = c.in.u30();
        std::vector<Value> cargs(argc); for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = c.pop();
        std::string mname = takeRuntimeName(c, mn);
        Value recv = c.pop();
        if (recv.kind == VKind::Object && recv.obj) c.push(c.vm.callMethod(recv.obj, mname, cargs));
        else c.push(Value::undefined()); });
    fvmOps().add(0x4e, [](ExecCtx& c) {  // callsupervoid
        uint32_t mn = c.in.u30(); uint32_t argc = c.in.u30();
        std::vector<Value> cargs(argc); for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = c.pop();
        std::string mname = takeRuntimeName(c, mn);
        Value recv = c.pop();
        if (recv.kind == VKind::Object && recv.obj) c.vm.callMethod(recv.obj, mname, cargs); });
    fvmOps().add(0x4c, [](ExecCtx& c) {  // callproplex
        uint32_t mn = c.in.u30(); uint32_t argc = c.in.u30();
        std::vector<Value> cargs(argc); for (uint32_t k = 0; k < argc; ++k) cargs[argc - 1 - k] = c.pop();
        std::string mname = takeRuntimeName(c, mn);
        Value objv = c.pop();
        if (objv.kind == VKind::Object && objv.obj) c.push(c.vm.callMethod(objv.obj, mname, cargs));
        else c.push(Value::undefined()); });
    fvmOps().add(0x32, [](ExecCtx& c) {  // hasnext2
        uint32_t objReg = c.in.u30(); uint32_t idxReg = c.in.u30();
        Value objv = (objReg < c.locals.size()) ? c.locals[objReg] : Value::undefined();
        int idx = (idxReg < c.locals.size()) ? c.locals[idxReg].toInt() : 0;
        auto keys = enumerableKeys(objv.kind == VKind::Object ? objv.obj : nullptr);
        if (idx >= 0 && idx < static_cast<int>(keys.size())) {
            if (idxReg < c.locals.size()) c.locals[idxReg] = Value::intg(idx + 1);
            c.push(Value::boolean(true));
        } else {
            if (idxReg < c.locals.size()) c.locals[idxReg] = Value::intg(0);
            c.push(Value::boolean(false));
        } });
    fvmOps().add(0x1e, [](ExecCtx& c) {  // nextname
        Value idxv = c.pop(); Value objv = c.pop();
        auto keys = enumerableKeys(objv.kind == VKind::Object ? objv.obj : nullptr);
        int i = idxv.toInt() - 1;
        c.push((i >= 0 && i < static_cast<int>(keys.size())) ? Value::str(keys[i]) : Value::undefined()); });
    fvmOps().add(0x23, [](ExecCtx& c) {  // nextvalue
        Value idxv = c.pop(); Value objv = c.pop();
        auto keys = enumerableKeys(objv.kind == VKind::Object ? objv.obj : nullptr);
        int i = idxv.toInt() - 1;
        if (i >= 0 && i < static_cast<int>(keys.size()) && objv.kind == VKind::Object && objv.obj)
            c.push(c.vm.getProperty(objv.obj, keys[i]));
        else c.push(Value::undefined()); });

    // ---- 第三批（A）：栈/swap/局部变量/作用域（仅依赖 stack/locals/scope）----
    fvmOps().add(0x2b, [](ExecCtx& c) {  // swap
        if (c.stack.size() >= 2) { Value a = c.pop(), b = c.pop(); c.push(a); c.push(b); } });
    fvmOps().add(0x62, [](ExecCtx& c) {  // getlocal
        uint32_t i2 = c.in.u30(); c.push(i2 < c.locals.size() ? c.locals[i2] : Value::undefined()); });
    fvmOps().add(0x63, [](ExecCtx& c) {  // setlocal
        uint32_t i2 = c.in.u30(); Value v = c.pop(); if (i2 < c.locals.size()) c.locals[i2] = v; });
    fvmOps().add(0xd0, [](ExecCtx& c) { c.push(c.locals.size() > 0 ? c.locals[0] : Value::undefined()); });
    fvmOps().add(0xd1, [](ExecCtx& c) { c.push(c.locals.size() > 1 ? c.locals[1] : Value::undefined()); });
    fvmOps().add(0xd2, [](ExecCtx& c) { c.push(c.locals.size() > 2 ? c.locals[2] : Value::undefined()); });
    fvmOps().add(0xd3, [](ExecCtx& c) { c.push(c.locals.size() > 3 ? c.locals[3] : Value::undefined()); });
    fvmOps().add(0xd4, [](ExecCtx& c) { Value v = c.pop(); if (!c.locals.empty()) c.locals[0] = v; });
    fvmOps().add(0xd5, [](ExecCtx& c) { Value v = c.pop(); if (c.locals.size() > 1) c.locals[1] = v; });
    fvmOps().add(0xd6, [](ExecCtx& c) { Value v = c.pop(); if (c.locals.size() > 2) c.locals[2] = v; });
    fvmOps().add(0xd7, [](ExecCtx& c) { Value v = c.pop(); if (c.locals.size() > 3) c.locals[3] = v; });
    fvmOps().add(0x30, [](ExecCtx& c) {  // pushscope
        Value v = c.pop(); if (v.kind == VKind::Object && v.obj) c.scope.push_back(v.obj); });
    fvmOps().add(0x1d, [](ExecCtx& c) { if (!c.scope.empty()) c.scope.pop_back(); });  // popscope

    // ---- 第三批（B）：常量 push（含 abc 常量池）----
    // pushshort：u30 编码的值按 s16 解释（Ruffle: read_u30() as i16）。
    fvmOps().add(0x25, [](ExecCtx& c) { c.push(Value::intg(static_cast<int16_t>(c.in.u30()))); });
    fvmOps().add(0x2c, [](ExecCtx& c) { uint32_t si2 = c.in.u30(); c.push(Value::str(c.vm.abcFile().str(si2))); });
    fvmOps().add(0x2d, [](ExecCtx& c) { uint32_t i2 = c.in.u30(); const auto& A = c.vm.abcFile(); c.push(Value::intg(i2 < A.ints.size() ? A.ints[i2] : 0)); });
    fvmOps().add(0x2e, [](ExecCtx& c) { uint32_t i2 = c.in.u30(); const auto& A = c.vm.abcFile(); c.push(Value::uintv(i2 < A.uints.size() ? A.uints[i2] : 0)); });
    fvmOps().add(0x2f, [](ExecCtx& c) { uint32_t i2 = c.in.u30(); const auto& A = c.vm.abcFile(); c.push(Value::num(i2 < A.doubles.size() ? A.doubles[i2] : 0.0)); });

    // ---- 第四批（A）：算术 / 位运算 / 比较 ----
    fvmOps().add(0xa0, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); if(a2.kind==VKind::String||b2.kind==VKind::String) c.push(Value::str(a2.toString()+b2.toString())); else c.push(Value::num(a2.toNumber()+b2.toNumber())); });
    fvmOps().add(0xa1, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::num(a2.toNumber()-b2.toNumber())); });
    fvmOps().add(0xa2, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::num(a2.toNumber()*b2.toNumber())); });
    fvmOps().add(0xa3, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); double d2=b2.toNumber(); c.push(Value::num(d2==0.0?NAN:a2.toNumber()/d2)); });
    fvmOps().add(0xa4, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); double d2=b2.toNumber(); c.push(Value::num(d2==0.0?NAN:std::fmod(a2.toNumber(),d2))); });
    fvmOps().add(0xa5, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::intg(a2.toInt()<<(b2.toInt()&31))); });
    fvmOps().add(0xa6, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::intg(a2.toInt()>>(b2.toInt()&31))); });
    fvmOps().add(0xa7, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::uintv(static_cast<uint32_t>(a2.toUInt()>>(b2.toUInt()&31)))); });
    fvmOps().add(0xa8, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::intg(a2.toInt()&b2.toInt())); });
    fvmOps().add(0xa9, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::intg(a2.toInt()|b2.toInt())); });
    fvmOps().add(0xaa, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::intg(a2.toInt()^b2.toInt())); });
    fvmOps().add(0xab, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(looseEquals(a2,b2))); });
    fvmOps().add(0xac, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(strictEquals(a2,b2))); });
    fvmOps().add(0xad, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(a2.toNumber()<b2.toNumber())); });
    fvmOps().add(0xae, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(a2.toNumber()<=b2.toNumber())); });
    fvmOps().add(0xaf, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(a2.toNumber()>b2.toNumber())); });
    fvmOps().add(0xb0, [](ExecCtx& c) { Value b2=c.pop(),a2=c.pop(); c.push(Value::boolean(a2.toNumber()>=b2.toNumber())); });

    // ---- 第五批（B）：属性 get/set + 一元/转换 ----
    fvmOps().add(0x66, [](ExecCtx& c) {  // getproperty: obj,[ns],[name] -> value
        uint32_t mn = c.in.u30();
        std::string name = takeRuntimeName(c, mn);
        Value obj = c.pop();
        Value r;
        if (obj.kind == VKind::Object && obj.obj) r = c.vm.getProperty(obj.obj, name);
        else if (obj.kind == VKind::String) r = c.vm.getProperty(c.vm.boxString(obj), name);
        else r = Value::undefined();
        if (std::getenv("SWF_AS_DEBUG") && obj.kind == VKind::Object && obj.obj &&
            obj.obj->kind == ObjKind::Array && !name.empty() &&
            name.find_first_not_of("0123456789") == std::string::npos) {
            static int cc = 0;
            if (cc < 20) { ++cc; std::fprintf(stderr, "[arrget] name='%s' r='%s'\n",
                                             name.c_str(), r.toString().c_str()); }
        }
        // SWF_AS_DYNGET=<子串>：按属性名跟踪动态 getproperty（含解析结果与具名子实例清单）。
        if (const char* dg = std::getenv("SWF_AS_DYNGET")) {
            if (name.find(dg) != std::string::npos) {
                static int dc = 0;
                if (dc++ < 400) {
                    std::string kids;
                    if (obj.kind == VKind::Object && obj.obj && obj.obj->display)
                        for (const auto& ch : obj.obj->display->children)
                            if (ch) kids += ch->name + "(cid" + std::to_string(ch->characterId) + ") ";
                    std::fprintf(stderr, "[dynget] self=%s disp=%d want='%s' r='%s' props=%d kids=[%s]\n",
                                 (obj.kind == VKind::Object && obj.obj) ? obj.obj->className.c_str() : "-",
                                 (obj.kind == VKind::Object && obj.obj && obj.obj->display) ? 1 : 0,
                                 name.c_str(), r.toString().c_str(),
                                 (obj.kind == VKind::Object && obj.obj) ? (int)obj.obj->props.count(name) : -1,
                                 kids.c_str());
                }
            }
        }
        c.push(r); });
    fvmOps().add(0x61, [](ExecCtx& c) {  // setproperty: obj,[ns],[name],value -> (空)
        uint32_t mn = c.in.u30();
        Value val = c.pop();
        std::string name = takeRuntimeName(c, mn);
        Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) c.vm.setProperty(obj.obj, name, val); });
    fvmOps().add(0x68, [](ExecCtx& c) {  // initproperty: obj,[ns],[name],value -> (空)
        uint32_t mn = c.in.u30();
        Value val = c.pop();
        std::string nm = takeRuntimeName(c, mn);
        Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) {
            obj.obj->props[nm] = val;
            for (auto cc = obj.obj->cls; cc; cc = cc->superClass) {
                bool found = false;
                for (const auto& m : cc->instanceMembers)
                    if (m.name == nm && (m.kind == Member::Slot || m.kind == Member::Const)) { obj.obj->slots[nm] = val; found = true; break; }
                if (found) break;
            }
        } });
    fvmOps().add(0x6c, [](ExecCtx& c) {  // getslot
        uint32_t slot = c.in.u30(); Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) {
            auto nm = obj.obj->props.find("$slot_" + std::to_string(slot));
            if (nm != obj.obj->props.end()) {
                const std::string sname = nm->second.toString();
                auto sp = obj.obj->slots.find(sname);
                if (sp != obj.obj->slots.end()) c.push(sp->second);
                else { auto pp = obj.obj->props.find(sname); c.push(pp != obj.obj->props.end() ? pp->second : Value::undefined()); }
            } else {
                auto p = obj.obj->props.find("$" + std::to_string(slot));
                c.push(p != obj.obj->props.end() ? p->second : Value::undefined());
            }
        } else c.push(Value::undefined()); });
    fvmOps().add(0x6d, [](ExecCtx& c) {  // setslot
        uint32_t slot = c.in.u30(); Value val = c.pop(); Value obj = c.pop();
        if (obj.kind == VKind::Object && obj.obj) {
            obj.obj->props["$" + std::to_string(slot)] = val;
            auto nm = obj.obj->props.find("$slot_" + std::to_string(slot));
            if (nm != obj.obj->props.end()) obj.obj->slots[nm->second.toString()] = val;
        } });
    fvmOps().add(0x90, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::num(-a2.toNumber())); });
    fvmOps().add(0x91, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::num(a2.toNumber()+1)); });
    fvmOps().add(0x93, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::num(a2.toNumber()-1)); });
    fvmOps().add(0x96, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::boolean(!a2.toBool())); });
    fvmOps().add(0x97, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::intg(~a2.toInt())); });
    fvmOps().add(0x73, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::intg(a2.toInt())); });
    fvmOps().add(0x74, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::uintv(a2.toUInt())); });
    fvmOps().add(0x75, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::num(a2.toNumber())); });
    fvmOps().add(0x76, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::boolean(a2.toBool())); });
    fvmOps().add(0x70, [](ExecCtx& c) { Value a2=c.pop(); c.push(Value::str(a2.toString())); });
    fvmOps().add(0x82, [](ExecCtx&) {});  // coerce_a (no-op)

    // ---- 第五批（C）：对象创建 / scope·global ----
    fvmOps().add(0x53, [](ExecCtx& c) {  // applytype
        uint32_t n = c.in.u30(); for (uint32_t k = 0; k <= n; ++k) c.pop();
        auto o = c.vm.newObject(ObjKind::Class, "Vector");
        auto arr = c.vm.globalsObj()->props.find("Array");
        if (arr != c.vm.globalsObj()->props.end() && arr->second.kind == VKind::Object && arr->second.obj) {
            o->classDef = arr->second.obj->classDef; o->cls = arr->second.obj->cls;
        }
        c.push(Value::object(o)); });
    fvmOps().add(0x55, [](ExecCtx& c) {  // newobject
        uint32_t n = c.in.u30(); auto o = c.vm.newObject(ObjKind::Generic, "Object");
        auto oc = c.vm.globalsObj()->props.find("Object");
        if (oc != c.vm.globalsObj()->props.end() && oc->second.kind == VKind::Object && oc->second.obj) o->cls = oc->second.obj->classDef;
        for (uint32_t k = 0; k < n; ++k) { Value v = c.pop(); Value key = c.pop(); o->props[key.toString()] = v; }
        c.push(Value::object(o)); });
    fvmOps().add(0x56, [](ExecCtx& c) {  // newarray
        uint32_t n = c.in.u30(); auto o = c.vm.newObject(ObjKind::Array, "Array");
        auto ac = c.vm.globalsObj()->props.find("Array");
        if (ac != c.vm.globalsObj()->props.end() && ac->second.kind == VKind::Object && ac->second.obj) o->cls = ac->second.obj->classDef;
        for (uint32_t k = 0; k < n; ++k) { Value v = c.pop(); o->props[std::to_string(n - 1 - k)] = v; }
        o->props["$length"] = Value::intg(static_cast<int32_t>(n)); c.push(Value::object(o)); });
    fvmOps().add(0x40, [](ExecCtx& c) {  // newfunction
        uint32_t mi = c.in.u30(); auto o = c.vm.newObject(ObjKind::Function, "Function");
        o->props["$method"] = Value::uintv(mi); o->scope = c.scope; c.push(Value::object(o)); });
    fvmOps().add(0x49, [](ExecCtx& c) {  // constructsuper：沿父类链执行超类实例构造
        uint32_t n = c.in.u30();
        std::vector<Value> cargs(n);
        for (uint32_t k = 0; k < n; ++k) cargs[n - 1 - k] = c.pop();
        Value recv = c.pop();
        auto obj = (recv.kind == VKind::Object && recv.obj) ? recv.obj : c.thisObj;
        if (!obj || !obj->cls || !obj->cls->superClass) return;
        auto sup = obj->cls->superClass;
        // 原生基类（Sprite/MovieClip/EventDispatcher/Event...）走其 ctor，
        // 否则按 ABC instance_info.iinit 执行超类字节码构造。
        if (sup->ctor) { sup->ctor(obj, cargs); return; }
        const auto& abc = c.vm.abcFile();
        if (sup->instanceIndex < 0 ||
            sup->instanceIndex >= static_cast<int>(abc.instances.size())) return;
        uint32_t init = abc.instances[sup->instanceIndex].iinit;
        if (init >= abc.methods.size()) return;
        int bi = abc.methods[init].bodyIndex;
        if (bi >= 0 && bi < static_cast<int>(abc.methodBodies.size()))
            c.vm.exec(abc.methodBodies[bi], cargs, obj);
    });
    fvmOps().add(0x64, [](ExecCtx& c) { c.push(Value::object(c.vm.globalsObj())); });  // getglobalscope
    fvmOps().add(0x65, [](ExecCtx& c) { uint32_t idx = c.in.u8(); c.push(idx < c.scope.size() ? Value::object(c.scope[idx]) : Value::undefined()); });  // getscopeobject

    // ---- 第六批：控制流（if/jump/return/throw/lookupswitch）----
    fvmOps().add(0x0C, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(!(a.toNumber()<b.toNumber())) c.jump(o); });   // ifnlt
    fvmOps().add(0x0D, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(!(a.toNumber()<=b.toNumber())) c.jump(o); });  // ifnle
    fvmOps().add(0x0E, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(!(a.toNumber()>b.toNumber())) c.jump(o); });   // ifngt
    fvmOps().add(0x0F, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(!(a.toNumber()>=b.toNumber())) c.jump(o); });  // ifnge
    fvmOps().add(0x10, [](ExecCtx& c) { int32_t o=c.in.s24(); c.jump(o); });                                          // jump
    fvmOps().add(0x11, [](ExecCtx& c) { int32_t o=c.in.s24(); if(c.pop().toBool()) c.jump(o); });                      // iftrue
    fvmOps().add(0x12, [](ExecCtx& c) { int32_t o=c.in.s24(); if(!c.pop().toBool()) c.jump(o); });                     // iffalse
    fvmOps().add(0x13, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop();
        if(looseEquals(a,b)) c.jump(o); });       // ifeq
    fvmOps().add(0x14, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop();
        if (std::getenv("SWF_AS_DEBUG") &&
            (a.toString().find("startFrame") != std::string::npos ||
             b.toString().find("startFrame") != std::string::npos)) {
            std::string as = a.toString(), bs = b.toString();
            std::fprintf(stderr, "[eq] ak=%d bk=%d a='%s' b='%s' eq=%d\n",
                         (int)a.kind, (int)b.kind, as.c_str(), bs.c_str(),
                         (int)looseEquals(a, b));
        }
        if(!looseEquals(a,b)) c.jump(o); });      // ifne
    fvmOps().add(0x15, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(a.toNumber()<b.toNumber()) c.jump(o); });  // iflt
    fvmOps().add(0x16, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(a.toNumber()<=b.toNumber()) c.jump(o); }); // ifle
    fvmOps().add(0x17, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(a.toNumber()>b.toNumber()) c.jump(o); });  // ifgt
    fvmOps().add(0x18, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(a.toNumber()>=b.toNumber()) c.jump(o); }); // ifge
    fvmOps().add(0x19, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(strictEquals(a,b)) c.jump(o); });     // ifstricteq
    fvmOps().add(0x1a, [](ExecCtx& c) { int32_t o=c.in.s24(); Value b=c.pop(),a=c.pop(); if(!strictEquals(a,b)) c.jump(o); });    // ifstrictne
    fvmOps().add(0x47, [](ExecCtx& c) { c.done=true; c.returnValue=Value::undefined(); });                            // returnvoid
    fvmOps().add(0x48, [](ExecCtx& c) { c.done=true; c.returnValue=c.pop(); });                                       // returnvalue
    // throw：交由 exec() 的 try/catch 处理，可跨方法向调用栈传播。
    fvmOps().add(0x03, [](ExecCtx& c) { throw AsThrow{c.pop()}; });
    fvmOps().add(0x1b, [](ExecCtx& c) {   // lookupswitch
        int32_t defOff = c.in.s24();
        uint32_t caseCount = c.in.u30();
        std::vector<int32_t> caseOffs(caseCount + 1);
        for (uint32_t k = 0; k <= caseCount; ++k) caseOffs[k] = c.in.s24();
        int32_t n = c.pop().toInt();
        int32_t chosen = defOff;
        if (n >= 0 && n <= static_cast<int32_t>(caseCount)) chosen = caseOffs[n];
        c.jump(chosen); });

    // ---- 第七批：调用 / 构造 / 类 / 属性查找 ----
    fvmOps().add(0x43, [](ExecCtx& c) {   // callmethod
        uint32_t mi=c.in.u30(), argc=c.in.u30();
        std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        Value recv=c.pop(); const auto& A=c.vm.abcFile();
        if(mi<A.methods.size()){
            int bi=A.methods[mi].bodyIndex;
            if(bi>=0&&bi<static_cast<int>(A.methodBodies.size()))
                c.push(c.vm.exec(A.methodBodies[bi],cargs,recv.kind==VKind::Object?recv.obj:nullptr));
            else c.push(Value::undefined());
        } else c.push(Value::undefined()); });
    fvmOps().add(0x44, [](ExecCtx& c) {   // callstatic
        uint32_t mi=c.in.u30(), argc=c.in.u30();
        std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        const auto& A=c.vm.abcFile();
        if(mi<A.methods.size()){
            int bi=A.methods[mi].bodyIndex;
            if(bi>=0&&bi<static_cast<int>(A.methodBodies.size()))
                c.push(c.vm.exec(A.methodBodies[bi],cargs,nullptr));
            else c.push(Value::undefined());
        } else c.push(Value::undefined()); });
    fvmOps().add(0x42, [](ExecCtx& c) {   // construct
        uint32_t argc=c.in.u30(); std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        Value cls=c.pop(); c.push(c.vm.constructObject(cls,cargs)); });
    fvmOps().add(0x4a, [](ExecCtx& c) {   // constructprop
        uint32_t mn=c.in.u30(), argc=c.in.u30();
        std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        std::string cname=takeRuntimeName(c, mn);
        Value base=c.pop();
        Value cls=(base.kind==VKind::Object&&base.obj)?c.vm.getProperty(base.obj,cname):Value::undefined();
        c.push(c.vm.constructObject(cls,cargs)); });
    fvmOps().add(0x46, [](ExecCtx& c) {   // callproperty
        uint32_t mn=c.in.u30(), argc=c.in.u30();
        std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        std::string mname=takeRuntimeName(c, mn);
        Value objv=c.pop();
        if(objv.kind==VKind::Object&&objv.obj){
            Value r=c.vm.callMethod(objv.obj,mname,cargs);
            if(mname=="addChild"&&objv.obj->display&&!objv.obj->display->parent){
                bool dup=false;
                for(auto& s:c.vm.stage) if(s.get()==objv.obj->display.get()){dup=true;break;}
                if(!dup) c.vm.stage.push_back(objv.obj->display);
            }
            c.push(r);
        } else if(objv.kind==VKind::String) c.push(c.vm.callMethod(c.vm.boxString(objv),mname,cargs));
        else c.push(Value::undefined()); });
    fvmOps().add(0x4f, [](ExecCtx& c) {   // callpropvoid
        uint32_t mn=c.in.u30(), argc=c.in.u30();
        std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        std::string mname=takeRuntimeName(c, mn);
        Value objv=c.pop();
        if(objv.kind==VKind::Object&&objv.obj){
            c.vm.callMethod(objv.obj,mname,cargs);
            if(mname=="addChild"&&objv.obj->display&&!objv.obj->display->parent){
                bool dup=false;
                for(auto& s:c.vm.stage) if(s.get()==objv.obj->display.get()){dup=true;break;}
                if(!dup) c.vm.stage.push_back(objv.obj->display);
            }
        } else if(objv.kind==VKind::String) c.vm.callMethod(c.vm.boxString(objv),mname,cargs); });
    fvmOps().add(0x41, [](ExecCtx& c) {   // call
        uint32_t argc=c.in.u30(); std::vector<Value> cargs(argc);
        for(uint32_t k=0;k<argc;++k) cargs[argc-1-k]=c.pop();
        Value thisv=c.pop(); Value fn=c.pop();
        c.push(c.vm.invokeValue(fn,cargs,thisv.kind==VKind::Object?thisv.obj:nullptr)); });
    fvmOps().add(0x58, [](ExecCtx& c) {   // newclass
        uint32_t ci=c.in.u30(); Value baseCls=c.pop();
        auto o=c.vm.newObject(ObjKind::Class,"Class");
        o->instanceIndex=static_cast<int>(ci);
        const auto& A=c.vm.abcFile();
        std::string cname=(ci<A.instances.size())?c.vm.qualifiedName(A.instances[ci].name):std::string();
        o->className=cname;
        if(ci<A.instances.size()){
            auto cd=c.vm.buildClass(static_cast<int>(ci));
            if(!cd->superClass&&baseCls.kind==VKind::Object&&baseCls.obj&&baseCls.obj->classDef)
                cd->superClass=baseCls.obj->classDef;
            o->classDef=cd; cd->classObject=o;
        }
        Value v=Value::object(o);
        if(!cname.empty()){
            c.vm.globalsObj()->props[cname]=v;
            size_t p=cname.rfind('.');
            if(p!=std::string::npos) c.vm.globalsObj()->props[cname.substr(p+1)]=v;
        }
        c.push(v); });
    { auto findProp=[](ExecCtx& c){
        uint32_t mn=c.in.u30(); std::string name=takeRuntimeName(c, mn);
        Value found=Value::nullv();
        for(auto it=c.scope.rbegin();it!=c.scope.rend();++it)
            if(declaresProperty(*it,name)){found=Value::object(*it);break;}
        if(found.isNull()) found=Value::object(c.vm.globalsObj());
        c.push(found); };
      fvmOps().add(0x5d,findProp); fvmOps().add(0x5e,findProp); }
    fvmOps().add(0x5f, [](ExecCtx& c) {   // finddef
        uint32_t mn=c.in.u30(); std::string name=takeRuntimeName(c, mn);
        auto& props=c.vm.globalsObj()->props;
        auto p=props.find(name);
        if(p==props.end()){ size_t dot=name.rfind('.'); if(dot!=std::string::npos) p=props.find(name.substr(dot+1)); }
        c.push(p==props.end()?Value::undefined():p->second); });
    fvmOps().add(0x60, [](ExecCtx& c) {   // getlex
        uint32_t mn=c.in.u30(); std::string name=takeRuntimeName(c, mn);
        Value found=Value::undefined();
        for(auto it=c.scope.rbegin();it!=c.scope.rend();++it)
            if(declaresProperty(*it,name)){found=c.vm.getProperty(*it,name);break;}
        if(found.isUndefined()) found=c.vm.getProperty(c.vm.globalsObj(),name);
        c.push(found); });

    // ---- 第八批：类型 / 转换 / 局部整数运算 / no-op ----
    fvmOps().add(0x86, [](ExecCtx& c) {   // astype: value -> value / null
        uint32_t mn=c.in.u30();
        std::string tn=takeRuntimeName(c, mn);
        auto type=findTypeObject(c.vm, tn);
        Value v=c.pop();
        c.push(valueIsType(v,type)?v:Value::nullv()); });
    fvmOps().add(0x87, [](ExecCtx& c) {   // astypelate: value,type -> value / null
        Value t=c.pop(); Value v=c.pop();
        c.push(valueIsType(v,t.kind==VKind::Object?t.obj:nullptr)?v:Value::nullv()); });
    fvmOps().add(0xb2, [](ExecCtx& c) {   // istype
        uint32_t mn=c.in.u30(); std::string name=takeRuntimeName(c, mn);
        Value v=c.pop();
        c.push(Value::boolean(valueIsType(v,findTypeObject(c.vm,name)))); });
    fvmOps().add(0xb3, [](ExecCtx& c) {   // istypelate
        Value t=c.pop(); Value v=c.pop();
        std::shared_ptr<Object> to = (t.kind==VKind::Object)?t.obj:nullptr;
        bool r = valueIsType(v, to);
        if (std::getenv("SWF_AS_DEBUG")) {
            static int c=0;
            if (c<30) { ++c; std::string tn = to?to->className:std::string("(null)");
                std::fprintf(stderr, "[istl] vk=%d tcn='%s' r=%d\n", (int)v.kind, tn.c_str(), (int)r); }
        }
        c.push(Value::boolean(r)); });
    fvmOps().add(0xb1, [](ExecCtx& c) {   // instanceof：沿类/原型链判定
        Value t=c.pop(); Value v=c.pop();
        c.push(Value::boolean(valueIsType(v,t.kind==VKind::Object?t.obj:nullptr))); });
    fvmOps().add(0xb4, [](ExecCtx& c) {   // in: name,obj -> bool
        Value obj=c.pop(); Value name=c.pop();
        bool has=(obj.kind==VKind::Object&&obj.obj)&&declaresProperty(obj.obj,name.toString());
        c.push(Value::boolean(has)); });
    fvmOps().add(0x95, [](ExecCtx& c) {   // typeof
        Value v=c.pop();
        switch(v.kind){
            case VKind::Undefined: c.push(Value::str("undefined")); break;
            case VKind::Null: c.push(Value::str("object")); break;
            case VKind::Bool: c.push(Value::str("boolean")); break;
            case VKind::Int: case VKind::UInt: case VKind::Number: c.push(Value::str("number")); break;
            case VKind::String: c.push(Value::str("string")); break;
            default: c.push(Value::str("object")); break;
        } });
    fvmOps().add(0x92, [](ExecCtx& c) { uint32_t i=c.in.u30(); if(i<c.locals.size()) c.locals[i]=Value::num(c.locals[i].toNumber()+1); });   // inclocal
    fvmOps().add(0x94, [](ExecCtx& c) { uint32_t i=c.in.u30(); if(i<c.locals.size()) c.locals[i]=Value::num(c.locals[i].toNumber()-1); });   // declocal
    fvmOps().add(0xc0, [](ExecCtx& c) { Value a=c.pop(); c.push(Value::intg(a.toInt()+1)); });   // increment_i
    fvmOps().add(0xc1, [](ExecCtx& c) { Value a=c.pop(); c.push(Value::intg(a.toInt()-1)); });   // decrement_i
    fvmOps().add(0xc2, [](ExecCtx& c) { uint32_t i=c.in.u30(); if(i<c.locals.size()) c.locals[i]=Value::intg(c.locals[i].toInt()+1); });
    fvmOps().add(0xc3, [](ExecCtx& c) { uint32_t i=c.in.u30(); if(i<c.locals.size()) c.locals[i]=Value::intg(c.locals[i].toInt()-1); });
    fvmOps().add(0xc4, [](ExecCtx& c) { Value a=c.pop(); c.push(Value::intg(-a.toInt())); });   // negate_i
    fvmOps().add(0xc5, [](ExecCtx& c) { Value b=c.pop(); Value a=c.pop(); c.push(Value::intg(a.toInt()+b.toInt())); });  // add_i
    fvmOps().add(0xc6, [](ExecCtx& c) { Value b=c.pop(); Value a=c.pop(); c.push(Value::intg(a.toInt()-b.toInt())); });  // subtract_i
    fvmOps().add(0xc7, [](ExecCtx& c) { Value b=c.pop(); Value a=c.pop(); c.push(Value::intg(a.toInt()*b.toInt())); });  // multiply_i
    fvmOps().add(0x71, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::str(xmlEscapeString(v.toString(),false))); });  // esc_xelem
    fvmOps().add(0x72, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::str(xmlEscapeString(v.toString(),true))); });   // esc_xattr
    fvmOps().add(0x80, [](ExecCtx& c) {   // coerce：按目标类型做基元转换，未知类型保持原值
        uint32_t mn=c.in.u30(); std::string tn=takeRuntimeName(c, mn);
        std::size_t dot=tn.rfind('.');
        std::string sn=(dot==std::string::npos)?tn:tn.substr(dot+1);
        Value v=c.pop();
        if(sn=="Boolean") c.push(Value::boolean(v.toBool()));
        else if(sn=="int") c.push(Value::intg(v.toInt()));
        else if(sn=="uint") c.push(Value::uintv(v.toUInt()));
        else if(sn=="Number") c.push(Value::num(v.toNumber()));
        else if(sn=="String") c.push(v.isNullOrUndef()?Value::nullv():Value::str(v.toString()));
        else if(sn=="Object"&&v.isNullOrUndef()) c.push(Value::nullv());
        else c.push(v); });
    fvmOps().add(0x81, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::boolean(v.toBool())); });       // coerce_b
    fvmOps().add(0x83, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::intg(v.toInt())); });          // coerce_i
    fvmOps().add(0x84, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::num(v.toNumber())); });        // coerce_d
    fvmOps().add(0x88, [](ExecCtx& c) { Value v=c.pop(); c.push(Value::uintv(v.toUInt())); });        // coerce_u
    fvmOps().add(0x89, [](ExecCtx& c) { Value v=c.pop(); c.push(v.isNullOrUndef()?Value::nullv():v); });  // coerce_o
    fvmOps().add(0x85, [](ExecCtx& c) { Value v=c.pop(); c.push(v.isNullOrUndef()?Value::nullv():Value::str(v.toString())); });  // coerce_s
    fvmOps().add(0x78, [](ExecCtx&) {});   // checkfilter: value -> value，保持栈顶
    fvmOps().add(0x01, [](ExecCtx&) {});   // bkpt
    fvmOps().add(0x06, [](ExecCtx& c) { c.in.u30(); });   // dxns
    fvmOps().add(0x07, [](ExecCtx& c) { c.pop(); });      // dxnslate: uriValue -> (空)
    fvmOps().add(0x08, [](ExecCtx& c) { c.in.u30(); });   // kill

    // ---- domainMemory（Alchemy / FP11+）：按 applicationDomain.domainMemory
    // 绑定的 ByteArray 做真实小端读写；未绑定时返回 0 / 丢弃写入。----
    for (int dmOp = 0x35; dmOp <= 0x39; ++dmOp)
        fvmOps().add(dmOp, [kind = dmOp - 0x35](ExecCtx& c) {
            const std::uint32_t addr = static_cast<std::uint32_t>(c.pop().toNumber());
            c.push(dmRead(c.vm, addr, kind));
        });
    for (int dmOp = 0x3A; dmOp <= 0x3E; ++dmOp)
        fvmOps().add(dmOp, [kind = dmOp - 0x3A](ExecCtx& c) {
            const std::uint32_t addr = static_cast<std::uint32_t>(c.pop().toNumber());
            const double val = c.pop().toNumber();
            dmWrite(c.vm, addr, kind, val);
        });

    // ---- 补齐：Flash IDE 编译产物常带的调试 opcode ----
    // 这些指令带操作数，若未注册而被"安全跳过"，操作数会被当成下一条指令，
    // 导致栈失衡、后续字节码全部错位（脚本看起来"执行了"但结果全错）。
    fvmOps().add(0xEF, [](ExecCtx& c) {   // debug: u8 type, u30 index, u8 reg, u30 extra
        c.in.u8(); c.in.u30(); c.in.u8(); c.in.u30(); });
    fvmOps().add(0xF0, [](ExecCtx& c) { c.in.u30(); });   // debugline
    fvmOps().add(0xF1, [](ExecCtx& c) { c.in.u30(); });   // debugfile
    fvmOps().add(0xF2, [](ExecCtx& c) { c.in.u30(); });   // bkptline
    fvmOps().add(0xF3, [](ExecCtx&) {});                   // timestamp

    // ---- 补齐：栈 / 作用域 / 全局槽 / 符号扩展 ----
    fvmOps().add(0x1F, [](ExecCtx& c) {   // hasnext: obj, index -> bool
        Value idx = c.pop(); Value obj = c.pop();
        bool more = false;
        if (obj.kind == VKind::Object && obj.obj)
            more = idx.toInt() < static_cast<int>(enumerableKeys(obj.obj).size());
        c.push(Value::boolean(more)); });
    fvmOps().add(0x31, [](ExecCtx& c) {   // pushnamespace: u30
        uint32_t i = c.in.u30();
        auto ns = c.vm.newObject(ObjKind::Generic, "Namespace");
        ns->props["$nsIndex"] = Value::uintv(i);
        c.push(Value::object(ns)); });
    fvmOps().add(0x6E, [](ExecCtx& c) {   // getglobalslot: u30
        uint32_t i = c.in.u30();
        auto& g = c.vm.globalsObj()->props;
        auto it = g.find("$globalslot_" + std::to_string(i));
        c.push(it == g.end() ? Value::undefined() : it->second); });
    fvmOps().add(0x6F, [](ExecCtx& c) {   // setglobalslot: u30
        uint32_t i = c.in.u30();
        c.vm.globalsObj()->props["$globalslot_" + std::to_string(i)] = c.pop(); });
    fvmOps().add(0x77, [](ExecCtx& c) {   // convert_o：不改变栈深
        if (c.stack.empty()) c.push(Value::undefined());
        else if (c.stack.back().isNullOrUndef()) c.stack.back() = Value::nullv(); });
    fvmOps().add(0x50, [](ExecCtx& c) {   // sxi1
        Value v = c.pop(); c.push(Value::intg((v.toInt() & 1) ? -1 : 0)); });
    fvmOps().add(0x51, [](ExecCtx& c) {   // sxi8
        Value v = c.pop(); c.push(Value::intg(static_cast<int8_t>(v.toInt()))); });
    fvmOps().add(0x52, [](ExecCtx& c) {   // sxi16
        Value v = c.pop(); c.push(Value::intg(static_cast<int16_t>(v.toInt()))); });
    fvmOps().add(0x67, [](ExecCtx& c) {   // getouterscope: u30
        uint32_t i = c.in.u30();
        c.push(i < c.scope.size() ? Value::object(c.scope[i]) : Value::undefined()); });
}

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
            // E4X：XML/XMLList 对象的字符串化结果是其 XML 文本，而不是 [object XML]。
            auto x = obj->props.find("$xmlRaw");
            if (x != obj->props.end()) return x->second.toString();
            return "[object " + (obj->className.empty() ? std::string("Object") : obj->className) + "]";
        }
        default: return "undefined";
    }
}

// 统计解释器未实现的 opcode。run() 只在初始化末尾汇总一次，而 tick() 期间
// 执行的帧脚本/定时器回调同样可能遇到未知指令，因此每帧结束也要汇总。
static std::map<int, long> g_unknownOps;
// SWF_AS_OPS 命中的帧脚本在执行期间打开 opcode 轨迹（跨递归 exec 生效）。
static bool g_traceOpsActive = false;
static void reportFvmUnknownOps(const char* phase) {
    if (!std::getenv("SWF_AS_DEBUG") || g_unknownOps.empty()) return;
    for (const auto& kv : g_unknownOps)
        std::fprintf(stderr, "[as] %s unimplemented op=0x%02X count=%ld\n",
                     phase, kv.first, kv.second);
    g_unknownOps.clear();
}

// 事件监听键：Event 对象取其 type（AS3 中 new Event(Event.ENTER_FRAME)），否则用字符串化结果。
static std::string listenerKey(const Value& v) {
    if (v.kind == VKind::Object && v.obj) {
        auto p = v.obj->props.find("type");
        if (p != v.obj->props.end()) return p->second.toString();
    }
    return v.toString();
}

// ---- AS3 全局函数的数值 / URI 辅助（不能用 toNumber() 代替 Number(v)）----
// Value::toNumber() 遇到不可解析字符串会返回 0，而 AS3 的 Number("abc") 是 NaN，
// 直接复用会让 isNaN("abc") / parseInt("12px") / Number(undefined) 全部算错。
static double asNumber(const Value& v) {
    switch (v.kind) {
        case VKind::Undefined: return NAN;
        case VKind::Null:      return 0.0;
        case VKind::Bool:      return v.toBool() ? 1.0 : 0.0;
        case VKind::Int:
        case VKind::UInt:
        case VKind::Number:    return v.toNumber();
        case VKind::Object:    return NAN;
        case VKind::String:    break;
    }
    std::string s = v.toString();
    const std::size_t p = s.find_first_not_of(" \t\r\n\f\v");
    if (p == std::string::npos) return 0.0;
    s = s.substr(p);
    const std::size_t e = s.find_last_not_of(" \t\r\n\f\v");
    if (e != std::string::npos) s = s.substr(0, e + 1);
    if (s.empty()) return 0.0;
    if (s == "Infinity" || s == "+Infinity") return INFINITY;
    if (s == "-Infinity") return -INFINITY;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char* end = nullptr;
        const long long n = std::strtoll(s.c_str() + 2, &end, 16);
        return (end == s.c_str() + 2) ? NAN : static_cast<double>(n);
    }
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    return (end == s.c_str() || *end != '\0') ? NAN : d;
}

static int digitVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return -1;
}

// parseInt(str, radix)：支持 0x 前缀、radix 2..36、尾部非数字字符、失败返回 NaN。
static double asParseInt(const std::string& raw, int radix) {
    const std::size_t p = raw.find_first_not_of(" \t\r\n\f\v");
    if (p == std::string::npos) return NAN;
    std::string s = raw.substr(p);
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) { neg = (s[0] == '-'); s.erase(0, 1); }
    int base = radix;
    if (base == 0) {
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
        else base = 10;
    } else if (base == 16 && s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }
    if (base < 2 || base > 36) return NAN;
    double acc = 0;
    bool any = false;
    for (char ch : s) {
        const int d = digitVal(ch);
        if (d < 0 || d >= base) break;
        acc = acc * base + d;
        any = true;
    }
    if (!any) return NAN;
    return neg ? -acc : acc;
}

// parseFloat(str)：解析数字前缀，Infinity/±Infinity，失败返回 NaN。
static double asParseFloat(const std::string& raw) {
    const std::size_t p = raw.find_first_not_of(" \t\r\n\f\v");
    if (p == std::string::npos) return NAN;
    const std::string s = raw.substr(p);
    if (s.compare(0, 8, "Infinity") == 0 || s.compare(0, 9, "+Infinity") == 0) return INFINITY;
    if (s.compare(0, 9, "-Infinity") == 0) return -INFINITY;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    return (end == s.c_str()) ? NAN : d;
}

// AS3 int/uint 是 32 位回绕转换，NaN/Infinity -> 0，向零截断。
static int32_t asIntCast(double d) {
    if (!std::isfinite(d)) return 0;
    d = std::trunc(d);
    const double k2 = 4294967296.0;
    d = std::fmod(d, k2);
    if (d < 0) d += k2;
    return static_cast<int32_t>(static_cast<uint32_t>(d));
}

static std::string uriEncode(const std::string& s, bool component) {
    static const char HEX[] = "0123456789ABCDEF";
    auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    std::string out;
    for (unsigned char ch : s) {
        bool safe = alnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
                    ch == '*' || ch == '\'' || ch == '(' || ch == ')' || ch == '!';
        if (!component)
            safe = safe || ch == '#' || ch == '$' || ch == '&' || ch == '+' || ch == ',' ||
                   ch == '/' || ch == ':' || ch == ';' || ch == '=' || ch == '?' || ch == '@';
        if (safe) out += static_cast<char>(ch);
        else { out += '%'; out += HEX[ch >> 4]; out += HEX[ch & 15]; }
    }
    return out;
}

static int hexVal(char c) {
    const int d = digitVal(c);
    return d < 0 || d >= 16 ? -1 : d;
}

static std::string uriDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += static_cast<char>((hexVal(s[i + 1]) << 4) | hexVal(s[i + 2]));
            i += 2;
        } else if (s[i] == '%' && i + 5 < s.size() && (s[i + 1] == 'u' || s[i + 1] == 'U') &&
                   hexVal(s[i + 2]) >= 0 && hexVal(s[i + 3]) >= 0 &&
                   hexVal(s[i + 4]) >= 0 && hexVal(s[i + 5]) >= 0) {
            const int cp = (hexVal(s[i + 2]) << 12) | (hexVal(s[i + 3]) << 8) |
                           (hexVal(s[i + 4]) << 4) | hexVal(s[i + 5]);
            if (cp < 128) out += static_cast<char>(cp); else out += s.substr(i, 6);
            i += 5;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- JSON.parse / JSON.stringify ----
static void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) out += static_cast<char>(cp);
    else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    char buf[16];
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) { std::snprintf(buf, sizeof(buf), "\\u%04X", ch); out += buf; }
                else out += static_cast<char>(ch);
        }
    }
    out += '"';
    return out;
}

static void jsonSkipWs(const std::string& s, std::size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}

static bool jsonParseString(const std::string& s, std::size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size()) {
        const char ch = s[p++];
        if (ch == '"') return true;
        if (ch == '\\') {
            if (p >= s.size()) return false;
            const char e = s[p++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (p + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int h = hexVal(s[p + static_cast<std::size_t>(k)]);
                        if (h < 0) return false;
                        cp = (cp << 4) | static_cast<unsigned>(h);
                    }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= s.size() &&
                        s[p] == '\\' && s[p + 1] == 'u') {
                        unsigned lo = 0;
                        bool good = true;
                        for (int k = 0; k < 4; ++k) {
                            const int h = hexVal(s[p + 2 + static_cast<std::size_t>(k)]);
                            if (h < 0) { good = false; break; }
                            lo = (lo << 4) | static_cast<unsigned>(h);
                        }
                        if (good && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            p += 6;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            return false;
        } else {
            out += ch;
        }
    }
    return false;
}

static bool jsonParseValue(Vm& vm, const std::string& s, std::size_t& p,
                           Value& out, int depth) {
    if (depth > 32) return false;
    jsonSkipWs(s, p);
    if (p >= s.size()) return false;
    const char ch = s[p];
    if (ch == '"') {
        std::string t;
        if (!jsonParseString(s, p, t)) return false;
        out = Value::str(t);
        return true;
    }
    if (ch == '{' || ch == '[') {
        const bool isArray = (ch == '[');
        ++p;
        auto o = vm.newObject(isArray ? ObjKind::Array : ObjKind::Generic,
                              isArray ? "Array" : "Object");
        jsonSkipWs(s, p);
        const char close = isArray ? ']' : '}';
        int n = 0;
        if (p < s.size() && s[p] == close) ++p;
        else {
            for (;;) {
                jsonSkipWs(s, p);
                std::string key;
                if (!isArray) {
                    if (!jsonParseString(s, p, key)) return false;
                    jsonSkipWs(s, p);
                    if (p >= s.size() || s[p] != ':') return false;
                    ++p;
                }
                Value v;
                if (!jsonParseValue(vm, s, p, v, depth + 1)) return false;
                o->props[isArray ? std::to_string(n++) : key] = v;
                jsonSkipWs(s, p);
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == close) { ++p; break; }
                return false;
            }
        }
        if (isArray) o->props["$length"] = Value::intg(n);
        out = Value::object(o);
        return true;
    }
    if (s.compare(p, 4, "true") == 0) { p += 4; out = Value::boolean(true); return true; }
    if (s.compare(p, 5, "false") == 0) { p += 5; out = Value::boolean(false); return true; }
    if (s.compare(p, 4, "null") == 0) { p += 4; out = Value::nullv(); return true; }
    char* end = nullptr;
    const double d = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    p = static_cast<std::size_t>(end - s.c_str());
    out = Value::num(d);
    return true;
}

// JSON.parse(text, reviver)：对每个 (key, value) 后序调用 AS3 函数值；返回
// undefined 的键按 AS3 语义删除。仅遍历公开属性，跳过 $ 开头的内部字段。
static bool jsonIsCallable(const Value& v) {
    return v.kind == VKind::Object && v.obj &&
           (v.obj->kind == ObjKind::Function || v.obj->props.count("$method") ||
            v.obj->props.count("$native"));
}
static Value jsonRevive(Vm& vm, const Value& reviver, const std::string& key,
                        const Value& v, int depth) {
    if (depth > 32) return v;
    Value cur = v;
    if (cur.kind == VKind::Object && cur.obj) {
        std::vector<std::string> keys;
        for (const auto& kv : cur.obj->props)
            if (!kv.first.empty() && kv.first[0] != '$') keys.push_back(kv.first);
        for (const auto& k : keys) {
            auto it = cur.obj->props.find(k);
            if (it == cur.obj->props.end()) continue;
            const Value r = jsonRevive(vm, reviver, k, it->second, depth + 1);
            if (r.isUndefined()) cur.obj->props.erase(k);
            else it->second = r;
        }
    }
    return vm.invokeValue(reviver, {Value::str(key), cur}, vm.globalsObj());
}

static std::string jsonNumber(double d) {
    if (!std::isfinite(d)) return "null";
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e21) std::snprintf(buf, sizeof(buf), "%.0f", d);
    else std::snprintf(buf, sizeof(buf), "%.17g", d);
    return buf;
}

// 返回空串表示该值应被跳过（undefined / 循环过深）。
static std::string jsonStringify(const Value& v, int depth) {
    if (depth > 32) return std::string();
    switch (v.kind) {
        case VKind::Undefined: return std::string();
        case VKind::Null:      return "null";
        case VKind::Bool:      return v.b ? "true" : "false";
        case VKind::Int:       return jsonNumber(static_cast<double>(v.i));
        case VKind::UInt:      return jsonNumber(static_cast<double>(v.u));
        case VKind::Number:    return jsonNumber(v.d);
        case VKind::String:    return jsonQuote(v.s);
        case VKind::Object:    break;
    }
    if (!v.obj) return "null";
    if (v.obj->kind == ObjKind::Array) {
        int n = 0;
        auto lp = v.obj->props.find("$length");
        if (lp != v.obj->props.end()) n = lp->second.toInt();
        std::string out = "[";
        for (int i = 0; i < n && i < 100000; ++i) {
            if (i) out += ',';
            const auto it = v.obj->props.find(std::to_string(i));
            const std::string e = it == v.obj->props.end()
                ? jsonStringify(Value::undefined(), depth + 1)
                : jsonStringify(it->second, depth + 1);
            out += e.empty() ? "null" : e;
        }
        out += ']';
        return out;
    }
    std::string out = "{";
    bool first = true;
    for (const auto& kv : v.obj->props) {
        if (!kv.first.empty() && kv.first[0] == '$') continue;   // 内部字段不序列化
        const std::string e = jsonStringify(kv.second, depth + 1);
        if (e.empty()) continue;
        if (!first) out += ',';
        first = false;
        out += jsonQuote(kv.first) + ":" + e;
    }
    out += '}';
    return out;
}

// Date 本地/UTC 分解（Windows 用 *_s，其他平台用 *_r）。
static std::tm dateTm(double ms, bool local) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000.0);
    std::tm out{};
#if defined(_WIN32)
    if (local) localtime_s(&out, &t); else gmtime_s(&out, &t);
#else
    if (local) localtime_r(&t, &out); else gmtime_r(&t, &out);
#endif
    return out;
}

static double dateTimeOf(const std::shared_ptr<Object>& self) {
    if (!self) return 0.0;
    const auto p = self->props.find("$time");
    return p == self->props.end() ? 0.0 : p->second.toNumber();
}

// ---- ByteArray 辅助 ----
static std::vector<std::uint8_t>& baBytes(const std::shared_ptr<Object>& self) {
    static std::vector<std::uint8_t> empty;
    if (!self) return empty;
    if (!self->bytes) self->bytes = std::make_shared<std::vector<std::uint8_t>>();
    return *self->bytes;
}
static std::size_t baPos(const std::shared_ptr<Object>& self) {
    if (!self) return 0;
    const auto p = self->props.find("position");
    return p == self->props.end() ? 0 : static_cast<std::size_t>(std::max(0, p->second.toInt()));
}
static void baSetPos(const std::shared_ptr<Object>& self, std::size_t v) {
    if (self) self->props["position"] = Value::intg(static_cast<std::int32_t>(v));
}
static Value baReadInt(const std::shared_ptr<Object>& self, int n, bool sign) {
    auto& b = baBytes(self);
    const std::size_t p = baPos(self);
    if (n <= 0 || n > 8 || p + static_cast<std::size_t>(n) > b.size()) return Value::undefined();
    std::uint64_t v = 0;
    for (int k = 0; k < n; ++k)
        v |= static_cast<std::uint64_t>(b[p + static_cast<std::size_t>(k)]) << (8 * k);
    baSetPos(self, p + static_cast<std::size_t>(n));
    if (sign) {
        if (n < 8) {
            const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8;
            const std::uint64_t signBit = 1ULL << (bits - 1);
            if (v & signBit) v |= ~((1ULL << bits) - 1);
        }
        return Value::intg(static_cast<std::int32_t>(static_cast<std::int64_t>(v)));
    }
    return Value::uintv(static_cast<std::uint32_t>(v));
}
static void baWriteInt(const std::shared_ptr<Object>& self, int n, double val) {
    if (n <= 0 || n > 8) return;
    auto& b = baBytes(self);
    std::size_t p = baPos(self);
    if (!std::isfinite(val)) val = 0.0;
    const std::uint64_t v = static_cast<std::uint64_t>(static_cast<std::int64_t>(val));
    if (p + static_cast<std::size_t>(n) > b.size()) b.resize(p + static_cast<std::size_t>(n), 0);
    for (int k = 0; k < n; ++k)
        b[p + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF);
    baSetPos(self, p + static_cast<std::size_t>(n));
}
static Value baReadFloat(const std::shared_ptr<Object>& self, bool dbl) {
    auto& b = baBytes(self);
    const std::size_t p = baPos(self);
    const std::size_t n = dbl ? 8 : 4;
    if (p + n > b.size()) return Value::undefined();
    if (dbl) { double v; std::memcpy(&v, b.data() + p, 8); baSetPos(self, p + n); return Value::num(v); }
    float v; std::memcpy(&v, b.data() + p, 4); baSetPos(self, p + n);
    return Value::num(static_cast<double>(v));
}
static void baWriteFloat(const std::shared_ptr<Object>& self, bool dbl, double val) {
    auto& b = baBytes(self);
    std::size_t p = baPos(self);
    const std::size_t n = dbl ? 8 : 4;
    if (p + n > b.size()) b.resize(p + n, 0);
    if (dbl) std::memcpy(b.data() + p, &val, 8);
    else { float f = static_cast<float>(val); std::memcpy(b.data() + p, &f, 4); }
    baSetPos(self, p + n);
}

// ---- 轻量 XML / E4X DOM ----
struct XmlNode {
    std::string name;
    std::string text;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;
};

static void xmlSkipWs(const std::string& s, std::size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}
static std::string xmlUnescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const std::size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 12) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") out += '&'; else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>'; else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const int base = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? 16 : 10;
            const char* digits = ent.c_str() + (base == 16 ? 2 : 1);
            const unsigned cp = static_cast<unsigned>(std::strtoul(digits, nullptr, base));
            if (cp < 128) out += static_cast<char>(cp); else out += '?';
        } else out += s.substr(i, e - i + 1);
        i = e;
    }
    return out;
}
static bool parseXmlElement(const std::string& s, std::size_t& p, XmlNode& out, int depth) {
    if (depth > 32) return false;
    xmlSkipWs(s, p);
    if (p >= s.size() || s[p] != '<') return false;
    if (s.compare(p, 4, "<!--") == 0) { const auto e = s.find("-->", p + 4); p = e == std::string::npos ? s.size() : e + 3; xmlSkipWs(s, p); }
    if (s.compare(p, 2, "<?") == 0) { const auto e = s.find("?>", p + 2); p = e == std::string::npos ? s.size() : e + 2; xmlSkipWs(s, p); }
    if (s.compare(p, 9, "<![CDATA[") == 0) {
        const auto e = s.find("]]>", p + 9);
        out.text += s.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
        p = e == std::string::npos ? s.size() : e + 3;
        return true;
    }
    if (p >= s.size() || s[p] != '<' || p + 1 >= s.size() || s[p + 1] == '/') return false;
    ++p;
    const std::size_t nameStart = p;
    while (p < s.size() && s[p] != '>' && s[p] != ' ' && s[p] != '\t' &&
           s[p] != '\r' && s[p] != '\n' && s[p] != '/') ++p;
    out.name = s.substr(nameStart, p - nameStart);
    for (;;) {
        xmlSkipWs(s, p);
        if (p >= s.size()) return false;
        if (s[p] == '>') { ++p; break; }
        if (s[p] == '/' && p + 1 < s.size() && s[p + 1] == '>') { p += 2; return true; }
        const std::size_t an = p;
        while (p < s.size() && s[p] != '=' && s[p] != ' ' && s[p] != '>' &&
               s[p] != '\t' && s[p] != '\n' && s[p] != '\r') ++p;
        const std::string aname = s.substr(an, p - an);
        xmlSkipWs(s, p);
        if (p < s.size() && s[p] == '=') {
            ++p; xmlSkipWs(s, p);
            if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                const char q = s[p++];
                const std::size_t vs = p;
                while (p < s.size() && s[p] != q) ++p;
                out.attrs[aname] = xmlUnescape(s.substr(vs, p - vs));
                if (p < s.size()) ++p;
            }
        } else if (!aname.empty()) out.attrs[aname] = "";
    }
    for (;;) {
        if (p >= s.size()) break;
        if (s[p] == '<') {
            if (s.compare(p, 2, "</") == 0) {
                const auto e = s.find('>', p + 2);
                p = e == std::string::npos ? s.size() : e + 1;
                break;
            }
            if (s.compare(p, 9, "<![CDATA[") == 0) {
                const auto e = s.find("]]>", p + 9);
                out.text += s.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
                p = e == std::string::npos ? s.size() : e + 3;
                continue;
            }
            if (s.compare(p, 4, "<!--") == 0) {
                const auto e = s.find("-->", p + 4);
                p = e == std::string::npos ? s.size() : e + 3;
                continue;
            }
            XmlNode child;
            if (!parseXmlElement(s, p, child, depth + 1)) { ++p; continue; }
            out.children.push_back(std::move(child));
        } else {
            const auto e = s.find('<', p);
            out.text += xmlUnescape(s.substr(p, e == std::string::npos ? std::string::npos : e - p));
            p = e == std::string::npos ? s.size() : e;
        }
    }
    return true;
}

// 把 DOM 节点转成运行时 XML 对象（不新增头文件结构，全部落在 Object::props）。
static Value xmlNodeToValue(Vm& vm, const XmlNode& n);
static Value xmlParseToValue(Vm& vm, const std::string& text) {
    std::size_t p = 0;
    XmlNode root;
    if (!parseXmlElement(text, p, root, 0)) {
        auto o = vm.newObject(ObjKind::XML, "XML");
        o->props["$xmlName"] = Value::str("");
        o->props["$xmlText"] = Value::str(text);
        return Value::object(o);
    }
    return xmlNodeToValue(vm, root);
}
static std::string xmlNodeSerialize(const XmlNode& n);

static Value xmlNodeToValue(Vm& vm, const XmlNode& n) {
    auto o = vm.newObject(ObjKind::XML, "XML");
    o->props["$xmlName"] = Value::str(n.name);
    o->props["$xmlText"] = Value::str(n.text);
    o->props["$xmlRaw"] = Value::str(xmlNodeSerialize(n));
    auto at = vm.newObject(ObjKind::Generic, "Object");
    for (const auto& kv : n.attrs) at->props[kv.first] = Value::str(kv.second);
    o->props["$xmlAttrs"] = Value::object(at);
    auto ch = vm.newObject(ObjKind::Array, "XMLList");
    for (std::size_t i = 0; i < n.children.size(); ++i)
        ch->props[std::to_string(i)] = xmlNodeToValue(vm, n.children[i]);
    ch->props["$length"] = Value::intg(static_cast<std::int32_t>(n.children.size()));
    o->props["$xmlChildren"] = Value::object(ch);
    return Value::object(o);
}
static std::string xmlNodeSerialize(const XmlNode& n) {
    std::string out = "<" + n.name;
    for (const auto& kv : n.attrs) out += " " + kv.first + "=\"" + kv.second + "\"";
    if (n.children.empty() && n.text.empty()) return out + "/>";
    out += ">" + n.text;
    for (const auto& c : n.children) out += xmlNodeSerialize(c);
    return out + "</" + n.name + ">";
}
static const Value* xmlChildValues(const std::shared_ptr<Object>& o, int& count) {
    count = 0;
    if (!o) return nullptr;
    const auto it = o->props.find("$xmlChildren");
    if (it == o->props.end() || it->second.kind != VKind::Object || !it->second.obj) return nullptr;
    const auto lp = it->second.obj->props.find("$length");
    count = lp == it->second.obj->props.end() ? 0 : lp->second.toInt();
    return &it->second;
}

static void xmlCollectDescendants(const std::shared_ptr<Object>& node,
                                  const std::string& want,
                                  std::vector<Value>& out, int depth) {
    if (!node || depth > 16 || out.size() > 4096) return;
    int n = 0;
    const Value* ch = xmlChildValues(node, n);
    if (!ch || !ch->obj) return;
    for (int i = 0; i < n; ++i) {
        auto it = ch->obj->props.find(std::to_string(i));
        if (it == ch->obj->props.end() || it->second.kind != VKind::Object || !it->second.obj) continue;
        const Value& child = it->second;
        auto nm = child.obj->props.find("$xmlName");
        const std::string cname = nm == child.obj->props.end() ? std::string() : nm->second.toString();
        if (want.empty() || want == "*" || cname == want) out.push_back(child);
        xmlCollectDescendants(child.obj, want, out, depth + 1);
    }
}

// 公历日期 -> 1970-01-01 的天数（Howard Hinnant days_from_civil）。允许
// 月份/日期溢出（setMonth(13)、setDate(0) 等 AS3 语义要求自动归一化）。
static std::int64_t daysFromCivil(int y, int m, int d) {
    const int q = (m >= 1) ? (m - 1) / 12 : -((12 - m) / 12);
    std::int64_t yy = static_cast<std::int64_t>(y) + q;
    const int mm = m - q * 12;
    yy -= (mm <= 2) ? 1 : 0;
    const std::int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const std::int64_t yoe = yy - era * 400;                       // [0, 399]
    const std::int64_t doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 +
                             static_cast<std::int64_t>(d) - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// 把分解后的日历字段重新组装成毫秒时间戳。UTC 分支用纯算术实现，
// 不依赖 timegm / _mkgmtime（不同 MinGW/MSVC 版本提供的名字不一致）。
static double dateAssemble(const std::tm& src, bool local, double millis) {
    if (local) {
        std::tm t = src;
        t.tm_isdst = -1;
        const std::time_t tt = std::mktime(&t);
        if (tt == static_cast<std::time_t>(-1))
            return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(tt) * 1000.0 + millis;
    }
    const std::int64_t secs =
        daysFromCivil(src.tm_year + 1900, src.tm_mon + 1, src.tm_mday) * 86400 +
        static_cast<std::int64_t>(src.tm_hour) * 3600 +
        static_cast<std::int64_t>(src.tm_min) * 60 + src.tm_sec;
    return static_cast<double>(secs) * 1000.0 + millis;
}

// which: 0=完整本地串 1=UTC 串 2=ISO 3=本地日期 4=本地时间 5/6=UTC 日期/时间
static std::string dateFormat(double ms, int which) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const bool utc = (which == 1 || which == 2 || which == 5 || which == 6);
    const std::tm t = dateTm(ms, !utc);
    const int wd = ((t.tm_wday % 7) + 7) % 7;
    const int mo = ((t.tm_mon % 12) + 12) % 12;
    const int frac = static_cast<int>(std::fmod(std::fmod(ms, 1000.0) + 1000.0, 1000.0));
    char buf[160];
    switch (which) {
        case 2:
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                          t.tm_year + 1900, mo + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, frac);
            break;
        case 1:
            std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                          kDays[wd], t.tm_mday, kMonths[mo], t.tm_year + 1900,
                          t.tm_hour, t.tm_min, t.tm_sec);
            break;
        case 3: case 5:
            std::snprintf(buf, sizeof(buf), "%s %s %02d %04d",
                          kDays[wd], kMonths[mo], t.tm_mday, t.tm_year + 1900);
            break;
        case 4: case 6:
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d GMT", t.tm_hour, t.tm_min, t.tm_sec);
            break;
        default:
            std::snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT",
                          kDays[wd], kMonths[mo], t.tm_mday, t.tm_year + 1900,
                          t.tm_hour, t.tm_min, t.tm_sec);
            break;
    }
    return buf;
}

static int dateField(const std::tm& t, int which) {
    switch (which) {
        case 0: return t.tm_year + 1900;
        case 1: return t.tm_mon;
        case 2: return t.tm_mday;
        case 3: return t.tm_wday;
        case 4: return t.tm_hour;
        case 5: return t.tm_min;
        case 6: return t.tm_sec;
        default: return 0;
    }
}

// ---- applicationDomain.domainMemory（Alchemy 指令 li*/lf*/si*/sf*）----
static bool dmInRange(const std::shared_ptr<std::vector<std::uint8_t>>& d,
                      std::uint32_t addr, int n) {
    return d && n > 0 && n <= 8 && static_cast<std::size_t>(addr) + n <= d->size();
}

// kind: 0=li8 1=li16 2=li32 3=lf32 4=lf64
static Value dmRead(Vm& vm, std::uint32_t addr, int kind) {
    static const int widths[] = {1, 2, 4, 4, 8};
    if (kind < 0 || kind > 4) return Value::num(0);
    auto d = vm.domainMemoryData();
    if (!dmInRange(d, addr, widths[kind])) return Value::num(0);
    const std::uint8_t* p = d->data() + addr;
    switch (kind) {
        case 0: return Value::intg(static_cast<std::int8_t>(p[0]));
        case 1: {
            std::uint16_t v; std::memcpy(&v, p, 2);
            return Value::intg(static_cast<std::int16_t>(v));
        }
        case 2: {
            std::uint32_t v; std::memcpy(&v, p, 4);
            return Value::intg(static_cast<std::int32_t>(v));
        }
        case 3: {
            float v; std::memcpy(&v, p, 4);
            return Value::num(static_cast<double>(v));
        }
        default: {
            double v; std::memcpy(&v, p, 8);
            return Value::num(v);
        }
    }
}

static void dmWrite(Vm& vm, std::uint32_t addr, int kind, double val) {
    static const int widths[] = {1, 2, 4, 4, 8};
    if (kind < 0 || kind > 4) return;
    auto d = vm.domainMemoryData();
    if (!dmInRange(d, addr, widths[kind])) return;
    std::uint8_t* p = d->data() + addr;
    switch (kind) {
        case 0: { std::uint8_t v = static_cast<std::uint8_t>(static_cast<std::int32_t>(val)); std::memcpy(p, &v, 1); break; }
        case 1: { std::uint16_t v = static_cast<std::uint16_t>(static_cast<std::int32_t>(val)); std::memcpy(p, &v, 2); break; }
        case 2: { std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(val)); std::memcpy(p, &v, 4); break; }
        case 3: { float v = static_cast<float>(val); std::memcpy(p, &v, 4); break; }
        default: { std::memcpy(p, &val, 8); break; }
    }
}

// 事件对象统一的内部字段（Event/TimerEvent/MouseEvent 等构造共用）。
static void initEventProps(const std::shared_ptr<Object>& self,
                           const Value& type, bool bubbles, bool cancelable) {
    if (!self) return;
    self->props["type"] = type;
    self->props["bubbles"] = Value::boolean(bubbles);
    self->props["cancelable"] = Value::boolean(cancelable);
    self->props["target"] = Value::nullv();
    self->props["currentTarget"] = Value::nullv();
    self->props["eventPhase"] = Value::intg(2);
    self->props["$defaultPrevented"] = Value::boolean(false);
    self->props["$stop"] = Value::boolean(false);
    self->props["$stopImmediate"] = Value::boolean(false);
}

// AS3 addChild/addChildAt 会先把 child 从原父容器摘除（跨容器移动），
// 且不允许同一 child 在 children 里出现两次导致重复绘制。
static void detachFromParent(const std::shared_ptr<DisplayObject>& child) {
    if (!child) return;
    if (auto p = child->parent) {
        auto& ch = p->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [&](const std::shared_ptr<DisplayObject>& c) {
                                    return c.get() == child.get();
                                }),
                 ch.end());
    }
    child->parent.reset();
}

// 两个监听器值是否指向同一函数（方法每次属性访问都会生成新函数对象）。
static bool sameListener(const Value& x, const Value& y) {
    if (x.kind != VKind::Object || !x.obj || y.kind != VKind::Object || !y.obj) return false;
    if (x.obj == y.obj) return true;
    const auto xm = x.obj->props.find("$method");
    const auto ym = y.obj->props.find("$method");
    if (xm != x.obj->props.end() || ym != y.obj->props.end()) {
        if (xm == x.obj->props.end() || ym == y.obj->props.end() ||
            xm->second.u != ym->second.u) return false;
    } else {
        const auto xn = x.obj->props.find("$native");
        const auto yn = y.obj->props.find("$native");
        if (xn == x.obj->props.end() || yn == y.obj->props.end() ||
            xn->second.toString() != yn->second.toString()) return false;
    }
    const auto xt = x.obj->props.find("$this");
    const auto yt = y.obj->props.find("$this");
    return xt != x.obj->props.end() && yt != y.obj->props.end() &&
           xt->second.kind == VKind::Object && yt->second.kind == VKind::Object &&
           xt->second.obj == yt->second.obj;
}

// 监听器优先级表与 listeners[key] 一一对应，存在 listeners["$prio:"+key]。
static std::vector<std::size_t> listenerOrder(const std::shared_ptr<Object>& self,
                                              const std::string& key, std::size_t n) {
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    const auto pit = self->listeners.find("$prio:" + key);
    if (pit == self->listeners.end() || pit->second.size() != n) return order;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return pit->second[a].toInt() > pit->second[b].toInt();
    });
    return order;
}

// 可枚举属性名（for each / for in）。数组按数字索引顺序，跳过内部 $ 标记。
static std::vector<std::string> enumerableKeys(const std::shared_ptr<Object>& o) {
    std::vector<std::string> keys;
    if (!o) return keys;
    if (o->kind == ObjKind::Array) {
        int n = 0;
        auto len = o->props.find("$length");
        if (len != o->props.end()) n = len->second.toInt();
        for (int i = 0; i < n; ++i) {
            std::string k = std::to_string(i);
            if (o->props.count(k)) keys.push_back(k);
        }
        return keys;
    }
    for (const auto& kv : o->props)
        if (kv.first.empty() || kv.first[0] != '$') keys.push_back(kv.first);
    for (const auto& kv : o->slots) keys.push_back(kv.first);
    return keys;
}

// 运行时名字 multiname（RTQName/MultinameL 等）：名字（或命名空间）由操作数栈提供。
// 运行时 multiname 需要从栈上弹出的额外值数量。
// RTQName/RTQNameA：只有 ns；RTQNameL/RTQNameLA：name + ns；MultinameL/LA：name。
static int mnRuntimeStackPops(const abc::AbcFile& abc, uint32_t index) {
    const abc::Multiname* m = abc.mn(index);
    if (!m) return 0;
    using K = abc::Multiname::Kind;
    switch (m->kind) {
        case K::RTQName:
        case K::RTQNameA:
        case K::MultinameL:
        case K::MultinameLA: return 1;
        case K::RTQNameL:
        case K::RTQNameLA:   return 2;
        default: return 0;
    }
}

static bool mnHasRuntimeNameValue(const abc::AbcFile& abc, uint32_t index) {
    const abc::Multiname* m = abc.mn(index);
    if (!m) return false;
    using K = abc::Multiname::Kind;
    return m->kind == K::RTQNameL || m->kind == K::RTQNameLA ||
           m->kind == K::MultinameL || m->kind == K::MultinameLA;
}

// 按全名 / 短名找全局类对象（coerce / astype / istype / instanceof 共用）。
static std::shared_ptr<Object> findTypeObject(Vm& vm, const std::string& name) {
    if (name.empty()) return nullptr;
    auto& props = vm.globalsObj()->props;
    auto p = props.find(name);
    if (p == props.end()) {
        const std::size_t dot = name.rfind('.');
        if (dot != std::string::npos) p = props.find(name.substr(dot + 1));
    }
    if (p != props.end() && p->second.kind == VKind::Object && p->second.obj)
        return p->second.obj;
    return nullptr;
}

// catch(TypeError e) 之类的类型匹配：按全名/短名检查异常对象类名与继承链。
static bool exceptionTypeMatches(const Value& ex, const std::string& want) {
    const std::size_t dot = want.rfind('.');
    const std::string shortWant = dot == std::string::npos ? want : want.substr(dot + 1);
    auto matches = [&](const std::string& n) {
        if (n == want) return true;
        const std::size_t d = n.rfind('.');
        return (d == std::string::npos ? n : n.substr(d + 1)) == shortWant;
    };
    if (shortWant == "*" || shortWant == "Object" || shortWant == "Error") return true;
    if (ex.kind == VKind::Object && ex.obj) {
        for (auto cd = ex.obj->cls; cd; cd = cd->superClass)
            if (matches(cd->name)) return true;
        return matches(ex.obj->className);
    }
    return false;
}

static std::string xmlEscapeString(const std::string& s, bool attribute) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  if (attribute) out += "&quot;"; else out += ch; break;
            case '\'': if (attribute) out += "&apos;"; else out += ch; break;
            default:   out += ch;
        }
    }
    return out;
}

// AS3 宽松相等（== / !=）：null 与 undefined 互相相等；数字与字符串按值转换比较。
static bool looseEquals(const Value& a, const Value& b) {
    if (a.isNullOrUndef() || b.isNullOrUndef())
        return a.isNullOrUndef() && b.isNullOrUndef();
    if (a.kind == VKind::Object || b.kind == VKind::Object) {
        if (a.kind == VKind::Object && b.kind == VKind::Object) return a.obj == b.obj;
        return a.toString() == b.toString();
    }
    if (a.kind == VKind::Bool && b.kind == VKind::Bool) return a.b == b.b;
    if (a.kind == VKind::String && b.kind == VKind::String) return a.s == b.s;
    return a.toNumber() == b.toNumber();
}

// AS3 严格相等（=== / !==）：类型（含 null 与 undefined 的区别）必须一致。
static bool strictEquals(const Value& a, const Value& b) {
    if (a.kind == VKind::Null || a.kind == VKind::Undefined ||
        b.kind == VKind::Null || b.kind == VKind::Undefined)
        return a.kind == b.kind;
    if (a.kind == VKind::Object || b.kind == VKind::Object)
        return a.kind == b.kind && a.obj == b.obj;
    if (a.kind != b.kind) {
        bool numA = a.kind == VKind::Int || a.kind == VKind::UInt || a.kind == VKind::Number;
        bool numB = b.kind == VKind::Int || b.kind == VKind::UInt || b.kind == VKind::Number;
        if (numA && numB) return a.toNumber() == b.toNumber();
        return false;
    }
    switch (a.kind) {
        case VKind::Bool:   return a.b == b.b;
        case VKind::Int:    return a.i == b.i;
        case VKind::UInt:   return a.u == b.u;
        case VKind::Number: return a.d == b.d;
        case VKind::String: return a.s == b.s;
        default:            return true;
    }
}

// AS3 `value is Type`（以及 `as Type` 的判定基础）：基础类型按值种类，对象按类继承链。
static bool valueIsType(const Value& v, const std::shared_ptr<Object>& t) {
    if (!t) return false;
    const std::string& tn = t->className;
    size_t dot = tn.rfind('.');
    std::string sn = (dot == std::string::npos) ? tn : tn.substr(dot + 1);
    if (sn == "int") return v.kind == VKind::Int;
    if (sn == "uint") return v.kind == VKind::UInt;
    if (sn == "Number") return v.kind == VKind::Number || v.kind == VKind::Int || v.kind == VKind::UInt;
    if (sn == "Boolean") return v.kind == VKind::Bool;
    if (sn == "String") return v.kind == VKind::String;
    if (v.kind != VKind::Object || !v.obj) return false;
    if (sn == "Object") return true;
    if (sn == "Array") return v.obj->kind == ObjKind::Array;
    auto shortName = [](const std::string& n) {
        std::size_t p = n.find_last_of(".:");
        return p == std::string::npos ? n : n.substr(p + 1);
    };
    auto matches = [&](const std::shared_ptr<ClassDef>& c) {
        if (!c) return false;
        if (t->classDef && c == t->classDef) return true;
        if (c->name == tn || shortName(c->name) == sn) return true;
        // AS3 接口：GameMain implements IStepFactory，接口不在 superClass 链上，
        // 但 `value as IStepFactory` 必须返回该实例（否则关卡工厂为 null）。
        for (const auto& in : c->interfaceNames)
            if (in == tn || shortName(in) == sn) return true;
        return false;
    };
    for (auto c = v.obj->cls; c; c = c->superClass)
        if (matches(c)) return true;
    for (auto c = v.obj->classDef; c; c = c->superClass)
        if (matches(c)) return true;
    // getChildByName / 时间轴具名子实例生成的包装对象只有 display、没有类链，
    // 但 Flash 里仍是 MovieClip 继承链上的显示对象。缺这一步时
    // `getChildByName("btnPlay") as InteractiveObject` 会得到 null，
    // endScript 里注册 CLICK 监听的两个分支被整段跳过（标题屏 PLAY 无响应）。
    const ObjKind k = v.obj->kind;
    const bool displayLike = v.obj->display || k == ObjKind::MovieClip || k == ObjKind::Sprite ||
        k == ObjKind::InteractiveObject || k == ObjKind::DisplayObjectContainer ||
        k == ObjKind::DisplayObject || k == ObjKind::TextField || k == ObjKind::Bitmap ||
        k == ObjKind::Shape || k == ObjKind::Stage || k == ObjKind::Loader;
    // VM 里 getChildByName / 时间轴子实例的包装对象没有细分 SimpleButton 子类型，
    // 这些对象本身就是按钮角色，as SimpleButton 应与 as InteractiveObject 一样放行。
    if (displayLike &&
        (sn == "EventDispatcher" || sn == "DisplayObject" || sn == "InteractiveObject" ||
         sn == "DisplayObjectContainer" || sn == "Sprite" || sn == "MovieClip" ||
         sn == "SimpleButton"))
        return true;
    return false;
}

// 对象是否声明了某属性（动态属性 / slot / 类实例成员 / 类静态成员）。
// findproperty/findpropstrict 需要按 trait 声明判断作用域对象，不能只看动态属性，
// 否则静态字段初始化会被错写到全局域。
static bool declaresProperty(const std::shared_ptr<Object>& o, const std::string& name) {
    if (!o) return false;
    if (o->props.count(name) || o->slots.count(name)) return true;
    for (auto c = o->cls; c; c = c->superClass)
        for (const auto& m : c->instanceMembers) if (m.name == name) return true;
    if (o->classDef) {
        for (const auto& m : o->classDef->instanceMembers) if (m.name == name) return true;
        for (const auto& m : o->classDef->staticMembers) if (m.name == name) return true;
        for (auto c = o->classDef->superClass; c; c = c->superClass)
            for (const auto& m : c->instanceMembers) if (m.name == name) return true;
    }
    return false;
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
        d->owner = o;
        o->display = d;
    }
    objects_.push_back(o);
    return o;
}

// AS3 类实例的显示类型：沿父类链寻找原生显示基类（AS3 类本身都是 Generic）。
static ObjKind displayKindOfClass(const std::shared_ptr<ClassDef>& cd) {
    for (auto c = cd; c; c = c->superClass) {
        const std::string& n = c->name;
        if (n == "MovieClip" || n == "flash.display.MovieClip") return ObjKind::MovieClip;
        if (n == "Sprite" || n == "flash.display.Sprite") return ObjKind::Sprite;
        if (n == "TextField" || n == "flash.text.TextField") return ObjKind::TextField;
        if (n == "Bitmap" || n == "flash.display.Bitmap") return ObjKind::Bitmap;
        if (n == "Shape" || n == "flash.display.Shape") return ObjKind::Shape;
        if (n == "Loader" || n == "flash.display.Loader") return ObjKind::Loader;
        if (n == "Stage" || n == "flash.display.Stage") return ObjKind::Stage;
    }
    return ObjKind::Generic;
}

// 按类对象创建实例并执行构造函数（construct / constructprop 共用）。
Value Vm::constructObject(const Value& cls, const std::vector<Value>& cargs) {
    ObjKind ok = ObjKind::Generic;
    std::string cname;
    auto cd = std::shared_ptr<ClassDef>();
    bool hasDef = false;
    if (cls.kind == VKind::Object && cls.obj) {
        auto* c0 = cls.obj.get();
        cname = c0->className;
        if (c0->classDef) {
            // 原生类
            cd = c0->classDef;
            hasDef = true;
            if (cname == "Array" || cname == "flash.utils.Array" || cname == "Vector") ok = ObjKind::Array;
            else if (cname == "String") ok = ObjKind::String;
            else if (cname == "Sprite" || cname == "flash.display.Sprite") ok = ObjKind::Sprite;
            else if (cname == "MovieClip" || cname == "flash.display.MovieClip") ok = ObjKind::MovieClip;
            else if (cname == "TextField") ok = ObjKind::TextField;
            else if (cname == "Timer") ok = ObjKind::Timer;
            // AS3 类：按继承链判断是否为显示对象（否则 MovieClip 子类拿不到 timeline，
            // 嵌套动画与 addFrameScript 都不会推进）
            else if (c0->instanceIndex >= 0) ok = displayKindOfClass(cd);
        } else if (c0->instanceIndex >= 0 && c0->instanceIndex < static_cast<int>(abc_.instances.size())) {
            cd = buildClass(c0->instanceIndex);
            ok = displayKindOfClass(cd);
        }
    }
    auto inst = newObject(ok, cname);
    inst->cls = cd;
    // 实例 slot 与按名属性统一存储：编译期 this.field 用 setslot(按 id)，跨类访问用
    // getproperty(按名)。构造时建立 slotId→名字 映射并初始化 slots[name]，
    // 使两种读写路径落在同一存储上（否则如 gameProgressBar 写入后按名读恒为 undefined）。
    if (cd) {
        for (auto c = cd; c; c = c->superClass)
            for (const auto& m : c->instanceMembers) {
                if ((m.kind != Member::Slot && m.kind != Member::Const) || m.slotId < 0) continue;
                std::string key = "$slot_" + std::to_string(m.slotId);
                if (!inst->props.count(key)) inst->props[key] = Value::str(m.name);
                if (!inst->slots.count(m.name)) inst->slots[m.name] = m.defaultValue;
            }
    }
    if (cd && hasDef) ensureClassInit(cd, cls.obj);   // 惰性静态初始化（依赖类此时均已定义）
    if (cd && cd->ctor) cd->ctor(inst, cargs);   // 原生类构造（Event/MouseEvent...）
    if (ok == ObjKind::Array) {
        // new Array(...) 的 Flash 语义：单参数且为数字表示长度，否则按元素列表填充。
        // 原实现忽略构造参数，new Array(name, frame) 会得到空数组，导致换装关 SHOW 后
        // Show[name] 解析不到目标影片剪辑、gotoAndStop 永不执行（衣服持续循环）。
        bool singleLen = cargs.size() == 1 &&
                         (cargs[0].kind == VKind::Number || cargs[0].kind == VKind::Int ||
                          cargs[0].kind == VKind::UInt);
        if (singleLen) {
            inst->props["$length"] = Value::intg(cargs[0].toInt());
        } else {
            for (size_t k = 0; k < cargs.size(); ++k) inst->props[std::to_string(k)] = cargs[k];
            inst->props["$length"] = Value::intg(static_cast<int32_t>(cargs.size()));
        }
    }
    // SymbolClass 关联
    auto sc = movie_.symbolClasses.find(cname);
    if (sc == movie_.symbolClasses.end()) {
        size_t p = cname.rfind('.');
        if (p != std::string::npos) sc = movie_.symbolClasses.find(cname.substr(p + 1));
    }
    if (sc != movie_.symbolClasses.end() && inst->display) inst->display->characterId = sc->second;
    // 执行实例构造函数
    if (cls.kind == VKind::Object && cls.obj && cls.obj->instanceIndex >= 0 &&
        cls.obj->instanceIndex < static_cast<int>(abc_.instances.size())) {
        uint32_t init = abc_.instances[cls.obj->instanceIndex].iinit;
        if (init < abc_.methods.size()) {
            int bi = abc_.methods[init].bodyIndex;
            if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                exec(abc_.methodBodies[bi], cargs, inst);
        }
    }
    return Value::object(inst);
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

// Slot/Const trait 的默认值：vindex 指向对应常量池，vkind 指定池种类。
// vkind 使用 ABC 常量池的类型标记：0x03=Int、0x04=UInt、0x06=Double、0x01=UTF8。
static Value traitDefaultValue(const abc::AbcFile& abc, const abc::Traits& t) {
    if (t.functionName == 0) return Value::undefined();   // 无默认值
    switch (t.vkind) {
        case 0x03: if (t.functionName < abc.ints.size()) return Value::intg(abc.ints[t.functionName]); break;
        case 0x04: if (t.functionName < abc.uints.size()) return Value::uintv(abc.uints[t.functionName]); break;
        case 0x06: if (t.functionName < abc.doubles.size()) return Value::num(abc.doubles[t.functionName]); break;
        case 0x01: if (t.functionName < abc.strings.size()) return Value::str(abc.strings[t.functionName]); break;
        default: break;
    }
    return Value::undefined();
}

// ---- 类构建 ----
void Vm::collectTraits(const std::vector<abc::Traits>& src, std::vector<Member>& out, bool isStatic) {
    for (const auto& t : src) {
        Member m;
        m.isStatic = isStatic;
        m.name = multinameName(t.name);
        m.slotId = static_cast<int>(t.slotId);
        m.methodIndex = t.functionName;
        m.defaultValue = traitDefaultValue(abc_, t);
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
    for (uint32_t iface : inst.interfaces) {
        std::string nm = qualifiedName(iface);
        if (!nm.empty()) cd->interfaceNames.push_back(nm);
    }

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
        // 父类不在 ABC 中时（Sprite / MovieClip / EventDispatcher 等原生类），
        // 链接到原生 ClassDef。否则 getProperty/callMethod 的父类链在原生基类处中断，
        // addChild / addEventListener / gotoAndPlay 等继承成员全部找不到（游戏逻辑无法执行）。
        // ABC 里父类名是限定名（flash.display.MovieClip），而原生类以短名注册，
        // 限定名查不到时必须按短名回退——否则继承链断在第一个用户类上，
        // 原生 getter（如 loaderInfo）对全部 fla 子类不可见。
        if (!cd->superClass) {
            auto np = globals_->props.find(supName);
            if (np == globals_->props.end() || np->second.kind != VKind::Object || !np->second.obj) {
                size_t dot = supName.rfind('.');
                if (dot != std::string::npos)
                    np = globals_->props.find(supName.substr(dot + 1));
            }
            if (np != globals_->props.end() && np->second.kind == VKind::Object && np->second.obj &&
                np->second.obj->classDef) {
                cd->superClass = np->second.obj->classDef;
            }
        }
    }

    collectTraits(inst.traits, cd->instanceMembers, false);
    if (instanceIndex < static_cast<int>(abc_.classes.size())) {
        collectTraits(abc_.classes[instanceIndex].traits, cd->staticMembers, true);
        cd->cinitMethod = abc_.classes[instanceIndex].cinit;
    }
    const char* clsFilter = std::getenv("SWF_AS_CLASS");
    if ((std::getenv("SWF_AS_DEBUG") &&
        (cd->name.find("FrameUtils") != std::string::npos || cd->name.find("MusicController") != std::string::npos ||
         cd->name.find("HandlerAction") != std::string::npos || cd->name.find("EventListener") != std::string::npos ||
         cd->name.find("MouseManager") != std::string::npos || cd->name.find("GameMainController") != std::string::npos ||
         cd->name.find("preloading") != std::string::npos || cd->name.find("Service") != std::string::npos ||
         cd->name == "com.wisp.webgame.API" || cd->name == "GameMain")) ||
        (clsFilter && cd->name.find(clsFilter) != std::string::npos)) {
        std::fprintf(stderr, "[as] class %s static=%zu idx=%d super=%d\n", cd->name.c_str(),
                     cd->staticMembers.size(), instanceIndex, cd->superInstanceIndex);
        for (const auto& m : cd->staticMembers)
            std::fprintf(stderr, "   static %s kind=%d mi=%u body=%d mname=%s\n", m.name.c_str(), m.kind, m.methodIndex,
                         (m.methodIndex < abc_.methods.size()) ? abc_.methods[m.methodIndex].bodyIndex : -1,
                         (m.methodIndex < abc_.methods.size()) ? abc_.methods[m.methodIndex].name.c_str() : "?");
        for (const auto& m : cd->instanceMembers)
            std::fprintf(stderr, "   inst %s kind=%d mi=%u mname=%s\n", m.name.c_str(), m.kind, m.methodIndex,
                         (m.methodIndex < abc_.methods.size()) ? abc_.methods[m.methodIndex].name.c_str() : "?");
    }
    return cd;
}

// ---- 类静态初始化 ----
void Vm::ensureClassInit(std::shared_ptr<ClassDef> cd, std::shared_ptr<Object> classObj) {
    if (!cd || cd->cinitDone) return;
    cd->cinitDone = true;                       // 先标记，避免递归
    if (!classObj) classObj = cd->classObject;
    if (cd->superClass) ensureClassInit(cd->superClass, cd->superClass->classObject);
    if (cd->cinitMethod < abc_.methods.size()) {
        int bi = abc_.methods[cd->cinitMethod].bodyIndex;
        if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
            exec(abc_.methodBodies[bi], {}, classObj);
    }
}

// ---- 属性访问（含类成员链与 getter/setter）----
Value Vm::getProperty(std::shared_ptr<Object> obj, const std::string& name) {
    return getPropertyImpl(obj, name);
}
Value Vm::getPropertyImpl(std::shared_ptr<Object> obj, const std::string& name) {
    if (!obj) return Value::undefined();
    // 类对象首次使用时才执行静态初始化（保证依赖的其他类已定义）
    if (obj->kind == ObjKind::Class && obj->classDef) ensureClassInit(obj->classDef, obj);
    // 1) 动态属性
    auto dp = obj->props.find(name);
    if (dp != obj->props.end()) {
        if (std::getenv("SWF_AS_DEBUG") && name == "name" && obj->props.count("frame")) {
            static int c = 0;
            if (c < 20) { ++c; std::fprintf(stderr, "[lbl-name] v=%s\n", dp->second.toString().c_str()); }
        }
        return dp->second;
    }
    // 2) slot
    auto sp = obj->slots.find(name);
    if (sp != obj->slots.end()) {
        // Flash 为文档类的每个时间轴实例自动生成同名实例 slot（如 pp1/pp2/pp3），
        // 由播放器在实例化时自动绑定。本引擎未预绑定：slot 值为 undefined 且显示树
        // 中存在同名具名子实例时，跳过 slot 交给下面 2.5 的具名子实例解析。
        bool bindChild = false;
        if (sp->second.isUndefined() && obj->display) {
            for (auto& c : obj->display->children)
                if (c && c->name == name) { bindChild = true; break; }
        }
        if (!bindChild) return sp->second;
    }
    // 2.1) 时间轴实例名：由 applyTimelineFrame 写入 display->name，未同步到动态属性，
    //      而 AS3 代码常用 clip.name（如 int(name.charAt(2))）区分同名元件实例。
    if (obj->display && name == "name") return Value::str(obj->display->name);
    // 2.5) 时间轴具名子实例（AS3 中 clip.instanceName 即属性访问）。
    //      统一走 objectForDisplay：它会把新建的包装回写 d->owner，保证同一显示对象
    //      只有一个 AS3 实例。否则帧脚本对 clip["gensui"] 注册的监听会在事件派发时
    //      落到另一个包装对象上而丢失（道具拖拽链失效）。
    if (obj->display) {
        for (auto& c : obj->display->children) {
            if (!c || c->name != name) continue;
            return Value::object(objectForDisplay(c));
        }
    }
    // 2.6) E4X：XML 对象上未显式定义的点号属性解析为同名子元素；
    // 单个命中返回 XML 节点本身，多个命中返回 XMLList。
    if (obj->props.count("$xmlChildren") && !name.empty() && name[0] != '@' && name != "*") {
        int n = 0;
        const Value* ch = xmlChildValues(obj, n);
        if (ch && ch->obj) {
            std::vector<Value> hits;
            for (int i = 0; i < n; ++i) {
                auto it = ch->obj->props.find(std::to_string(i));
                if (it == ch->obj->props.end() || !it->second.obj) continue;
                auto nm = it->second.obj->props.find("$xmlName");
                if (nm != it->second.obj->props.end() && nm->second.toString() == name)
                    hits.push_back(it->second);
            }
            if (hits.size() == 1) return hits[0];
            if (hits.size() > 1) {
                auto l = newObject(ObjKind::Array, "XMLList");
                for (std::size_t k = 0; k < hits.size(); ++k) l->props[std::to_string(k)] = hits[k];
                l->props["$length"] = Value::intg(static_cast<int32_t>(hits.size()));
                return Value::object(l);
            }
        }
    }
    // 3) 类成员链（实例成员 -> 静态成员 -> 父类）
    for (auto c = obj->cls; c; c = c->superClass) {
        for (const auto& m : c->instanceMembers) {
            if (m.name != name) continue;
            if (m.kind == Member::Getter) {
                if (m.native) return m.native(obj, {});
                // ABC getter：执行 getter 方法体并返回其结果
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size())) {
                        Value r = exec(abc_.methodBodies[bi], {}, obj);
                        if (std::getenv("SWF_AS_DEBUG"))
                            std::fprintf(stderr, "[as] GET %s mi=%u body=%d -> %s\n", name.c_str(),
                                         m.methodIndex, bi, r.toString().c_str());
                        return r;
                    }
                } else if (std::getenv("SWF_AS_DEBUG")) {
                    std::fprintf(stderr, "[as] GET %s mi=%u OUT-OF-RANGE methods=%zu\n",
                                 name.c_str(), m.methodIndex, abc_.methods.size());
                }
                return Value::undefined();
            }
            if (m.kind == Member::Method) {
                auto fn = newObject(ObjKind::Function, name);
                if (m.native) fn->props["$native"] = Value::str(name);   // 原生方法（含继承）
                else fn->props["$method"] = Value::uintv(m.methodIndex);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
            if (m.kind == Member::Slot || m.kind == Member::Const) return m.defaultValue;
            if (m.defaultValue.kind != VKind::Undefined) return m.defaultValue;
        }
        for (const auto& m : c->staticMembers) {
            if (m.name != name) continue;
            if (m.kind == Member::Getter) {
                if (m.native) return m.native(obj, {});
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                        return exec(abc_.methodBodies[bi], {}, obj);
                }
                return Value::undefined();
            }
            if (m.kind == Member::Method && m.native) {
                auto fn = newObject(ObjKind::Function, name);
                fn->props["$native"] = Value::str(name);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
            if (m.defaultValue.kind != VKind::Undefined) return m.defaultValue;   // 静态常量
            if (m.kind == Member::Method) {
                auto fn = newObject(ObjKind::Function, name);
                fn->props["$method"] = Value::uintv(m.methodIndex);
                fn->props["$this"] = Value::object(obj);
                return Value::object(fn);
            }
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
        for (const auto& m : obj->classDef->staticMembers)
            if (m.name == name) {
                if (m.kind == Member::Getter && m.native) return m.native(obj, {});
                if (m.kind == Member::Method && m.native) {
                    auto fn = newObject(ObjKind::Function, name);
                    fn->props["$native"] = Value::str(name);
                    fn->props["$this"] = Value::object(obj);
                    return Value::object(fn);
                }
                if (m.defaultValue.kind != VKind::Undefined) return m.defaultValue;
            }
    }
    return Value::undefined();
}

void Vm::setProperty(std::shared_ptr<Object> obj, const std::string& name, const Value& v) {
    if (!obj) return;
    // applicationDomain.domainMemory = ByteArray：Alchemy 的 li*/si*/lf*/sf*
    // 直接读写这个 ByteArray 的字节缓冲，两者必须共享同一块存储。
    if (name == "domainMemory" && v.kind == VKind::Object && v.obj) {
        if (!v.obj->bytes) v.obj->bytes = std::make_shared<std::vector<std::uint8_t>>();
        domainMemoryData_ = v.obj->bytes;
    }
    // 类对象首次使用时执行静态初始化（与读取/调用一致；否则 cinit 会覆盖先写入的静态字段）
    if (obj->kind == ObjKind::Class && obj->classDef) ensureClassInit(obj->classDef, obj);
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
        // 脚本写入几何属性：标记为由脚本接管，静止的时间轴放置记录不再每帧覆盖。
        if (name == "x" || name == "y" || name == "scaleX" || name == "scaleY" ||
            name == "rotation" || name == "alpha")
            obj->display->scriptTransform = true;
        if (name == "x") { obj->display->x = v.toNumber(); obj->props[name] = v; return; }
        if (name == "y") { obj->display->y = v.toNumber(); obj->props[name] = v; return; }
        if (name == "scaleX") { obj->display->scaleX = v.toNumber(); obj->display->hasFull = false; obj->props[name] = v; return; }
        if (name == "scaleY") { obj->display->scaleY = v.toNumber(); obj->display->hasFull = false; obj->props[name] = v; return; }
        if (name == "rotation") { obj->display->rotation = v.toNumber(); obj->display->hasFull = false; obj->props[name] = v; return; }
        if (name == "alpha") {
            obj->display->alpha = v.toNumber(); obj->props[name] = v;
            if (std::getenv("SWF_AS_DEBUG") && obj->display->name == "hit")
                std::fprintf(stderr, "[as] hit.alpha = %.4f\n", v.toNumber());
            return;
        }
        if (name == "visible") { obj->display->visible = v.toBool(); obj->props[name] = v; return; }
        if (name == "mouseEnabled") { obj->display->mouseEnabled = v.toBool(); obj->props[name] = v; return; }
        if (name == "mouseChildren") { obj->display->mouseChildren = v.toBool(); obj->props[name] = v; return; }
        if (name == "name") {
            obj->display->name = v.toString(); obj->props[name] = v;
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] set name='%s' on %s\n", v.toString().c_str(), obj->className.c_str());
            return;
        }
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
        if (name == "blendMode" && obj->display) {
            BlendMode bm = BlendMode::Normal;
            if (v.kind == VKind::String) {
                std::string s = v.toString();
                if (s == "add") bm = BlendMode::Add;
                else if (s == "multiply")   bm = BlendMode::Multiply;
                else if (s == "screen")     bm = BlendMode::Screen;
                else if (s == "lighten")    bm = BlendMode::Lighten;
                else if (s == "darken")     bm = BlendMode::Darken;
                else if (s == "difference") bm = BlendMode::Difference;
                else if (s == "subtract")   bm = BlendMode::Subtract;
                else if (s == "invert")     bm = BlendMode::Invert;
                else if (s == "layer")      bm = BlendMode::Layer;
                else if (s == "alpha")      bm = BlendMode::Alpha;
                else if (s == "erase")      bm = BlendMode::Erase;
                else if (s == "overlay")    bm = BlendMode::Overlay;
                else if (s == "hardlight")  bm = BlendMode::HardLight;
                else bm = BlendMode::Normal;
            } else {
                int n = static_cast<int>(v.toUInt());
                if (n >= 0 && n <= 13) bm = static_cast<BlendMode>(n);
            }
            obj->display->blend = bm;
            obj->props[name] = v; return;
        }
    }
    // 声明为 slot 的成员：同步写入 slots[name]，保证 getslot(按 id) 读到最新值
    for (auto c = obj->cls; c; c = c->superClass) {
        bool found = false;
        for (const auto& m : c->instanceMembers)
            if (m.name == name && (m.kind == Member::Slot || m.kind == Member::Const)) {
                obj->slots[name] = v; found = true; break;
            }
        if (found) break;
    }
    obj->props[name] = v;
}

// 调用 AS3 函数对象：闭包按捕获的作用域执行；原生绑定函数转调对应方法。
Value Vm::invokeValue(const Value& fn, const std::vector<Value>& args,
                      std::shared_ptr<Object> thisObj) {
    if (fn.kind != VKind::Object || !fn.obj) return Value::undefined();
    auto& f = fn.obj;
    auto ti = f->props.find("$this");
    if (ti != f->props.end() && ti->second.kind == VKind::Object && ti->second.obj)
        thisObj = ti->second.obj;
    auto mi = f->props.find("$method");
    if (mi != f->props.end()) {
        uint32_t idx = mi->second.u;
        if (std::getenv("SWF_AS_DEBUG"))
            std::fprintf(stderr, "[as] invokeValue method=%u body=%d class=%s scope=%zu\n", idx,
                         (idx < abc_.methods.size()) ? abc_.methods[idx].bodyIndex : -1,
                         f->className.c_str(), f->scope.size());
        // SWF_AS_OPS 在 invokeValue 层也匹配：闭包执行时 thisObj 会被 $this 覆盖，
        // 仅靠 exec/callMethod 的 thisObj 类名匹配会漏掉方法名为空、但闭包名为
        // onServiceReady 这类监听处理器；这里按闭包名+方法名双匹配并打开 traceOps。
        bool tracedHere = false;
        if (const char* flt = std::getenv("SWF_AS_OPS")) {
            bool m = f->className.find(flt) != std::string::npos;
            if (!m && idx < abc_.methods.size()) m = abc_.methods[idx].name.find(flt) != std::string::npos;
            if (m) { g_traceOpsActive = true; tracedHere = true; }
        }
        if (idx < abc_.methods.size()) {
            int bi = abc_.methods[idx].bodyIndex;
            if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size())) {
                Value r = exec(abc_.methodBodies[bi], args, thisObj, &f->scope);
                if (tracedHere) g_traceOpsActive = false;
                return r;
            }
        }
        if (tracedHere) g_traceOpsActive = false;
        return Value::undefined();
    }
    auto ni = f->props.find("$native");
    if (ni != f->props.end() && thisObj)
        return callMethod(thisObj, ni->second.toString(), args);
    return Value::undefined();
}

std::shared_ptr<Object> Vm::boxString(const Value& v) {
    auto so = newObject(ObjKind::String, "String");
    so->props["$str"] = v;
    if (globals_) {
        auto gi = globals_->props.find("String");
        if (gi != globals_->props.end() && gi->second.kind == VKind::Object && gi->second.obj)
            so->cls = gi->second.obj->classDef;
    }
    return so;
}

Value Vm::callMethod(std::shared_ptr<Object> obj, const std::string& name, const std::vector<Value>& args) {
    if (!obj) return Value::undefined();
    if (obj->kind == ObjKind::Class && obj->classDef) ensureClassInit(obj->classDef, obj);
    if (std::getenv("SWF_AS_DEBUG")) {
        std::string cn = obj->className;
        if (obj->classDef) cn = obj->classDef->name;
        if (cn.find("FrameUtils") != std::string::npos) {
            std::string argstr;
            for (const auto& a : args) argstr += " " + a.toString();
            std::fprintf(stderr, "[fu] m=%s args=%s\n", name.c_str(), argstr.c_str());
            if (name == "getFrameIndexByLabel") {
                if (args.size() >= 2)
                    std::fprintf(stderr, "[fu-args] label kind=%d val='%s'\n",
                                 (int)args[1].kind, args[1].toString().c_str());
                for (size_t mi = 0; mi < abc_.methods.size(); ++mi) {
                    if (abc_.methods[mi].bodyIndex == 55 &&
                        abc_.methods[mi].bodyIndex >= 0 &&
                        static_cast<std::size_t>(abc_.methods[mi].bodyIndex) < abc_.methodBodies.size()) {
                        std::fprintf(stderr, "[fu-match] mi=%zu name='%s'\n", mi,
                                     abc_.methods[mi].name.c_str());
                        const auto& body = abc_.methodBodies[abc_.methods[mi].bodyIndex];
                        std::string out;
                        for (std::size_t pc = 0; pc < body.code.size();) {
                            avm2::Instruction ins = avm2::decode(body.code.data(), body.code.size(), pc);
                            if (ins.size == 0) break;
                            const char* nm = ins.info && ins.info->valid() ? ins.info->name : "db";
                            out += nm;
                            if (ins.info && ins.info->nameOperand != avm2::kNoOperand &&
                                ins.info->nameOperand < ins.operands.size()) {
                                uint32_t mn = static_cast<uint32_t>(ins.operands[ins.info->nameOperand]);
                                std::string lbl = (mn < abc_.multinames.size())
                                    ? abc_.str(abc_.multinames[mn].name) : "?";
                                out += " [" + lbl + "]";
                            }
                            if (ins.code == 0x2C && !ins.operands.empty()) {
                                uint32_t si = static_cast<uint32_t>(ins.operands[0]);
                                out += " '" + (si < abc_.strings.size() ? abc_.strings[si] : std::string("?")) + "'";
                            }
                            for (std::size_t k = 0; k < ins.operands.size(); ++k) {
                                uint32_t op = static_cast<uint32_t>(ins.operands[k]);
                                bool handled = false;
                                if (ins.info && ins.info->nameOperand != avm2::kNoOperand &&
                                    ins.info->nameOperand == static_cast<int>(k)) {
                                    std::string lbl = (op < abc_.multinames.size())
                                        ? abc_.str(abc_.multinames[op].name) : "?";
                                    out += " [" + lbl + "]"; handled = true;
                                } else if (ins.code == 0x2C && k == 0) {
                                    out += " '" + (op < abc_.strings.size() ? abc_.strings[op] : std::string("?")) + "'";
                                    handled = true;
                                }
                                if (!handled) out += " " + std::to_string(op);
                            }
                            out += "\n";
                            pc += ins.size;
                        }
                        std::fprintf(stderr, "[fu-asm]\n%s", out.c_str());
                    }
                }
            }
        }
    }
    if (std::getenv("SWF_AS_DEBUG") && (name == "add" || name == "send")) {
        std::fprintf(stderr, "[as] %s.%s args=", obj->className.c_str(), name.c_str());
        for (auto& a : args) std::fprintf(stderr, "[%s]", a.toString().c_str());
        auto md = obj->props.find("m_data");
        if (md != obj->props.end() && md->second.kind == VKind::Object && md->second.obj) {
            std::fprintf(stderr, " m_data(%s,props=%zu)", md->second.obj->className.c_str(),
                         md->second.obj->props.size());
            for (auto& kv : md->second.obj->props)
                std::fprintf(stderr, " {%s:%s}", kv.first.c_str(), kv.second.toString().c_str());
        }
        std::fprintf(stderr, "\n");
    }
    // Function.call / Function.apply
    if ((name == "call" || name == "apply") &&
        (obj->props.count("$method") || obj->props.count("$native"))) {
        std::shared_ptr<Object> th;
        std::vector<Value> a2;
        if (!args.empty() && args[0].kind == VKind::Object) th = args[0].obj;
        if (name == "call") {
            a2.assign(args.begin() + (args.empty() ? 0 : 1), args.end());
        } else if (args.size() > 1 && args[1].kind == VKind::Object && args[1].obj) {
            auto& arr = args[1].obj->props;
            int n = 0;
            auto len = arr.find("$length");
            if (len != arr.end()) n = len->second.toInt();
            for (int i = 0; i < n; ++i) {
                auto it = arr.find(std::to_string(i));
                if (it != arr.end()) a2.push_back(it->second);
            }
        }
        return invokeValue(Value::object(obj), a2, th);
    }
    // 0) 全局内建函数
    // 只对全局对象 / 无同名成员的普通作用域对象生效，避免劫持自定义类的同名方法。
    const bool globalCall = (obj == globals_) ||
        (obj->kind != ObjKind::Class && !obj->classDef &&
         !obj->props.count(name) && !obj->slots.count(name));
    if (globalCall && name == "trace") {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) std::printf(" ");
            std::printf("%s", args[i].toString().c_str());
        }
        std::printf("\n");
        return Value::undefined();
    }
    if (globalCall && (name == "setTimeout" || name == "setInterval")) {
        if (args.size() >= 2) {
            TimerTask t;
            t.id = nextTimerId_++;
            t.dueMs = timerNowMs_ + args[1].toNumber();
            t.intervalMs = (name == "setInterval") ? std::max(1.0, args[1].toNumber()) : 0.0;
            t.fn = args[0];
            t.args.assign(args.begin() + 2, args.end());
            timers_.push_back(t);
            return Value::intg(t.id);
        }
        return Value::intg(0);
    }
    if (globalCall && (name == "clearTimeout" || name == "clearInterval")) {
        const int id = args.empty() ? 0 : args[0].toInt();
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [id](const TimerTask& t) { return t.id == id; }),
                      timers_.end());
        return Value::undefined();
    }
    // getTimer()：自影片开始以来的毫秒数（与 setTimeout 使用同一虚拟时钟，保证逐帧可复现）
    if (globalCall && name == "getTimer") return Value::intg(static_cast<int32_t>(timerNowMs_));
    if (globalCall && !args.empty()) {
        const Value& v = args[0];
        if (name == "parseInt") {
            const int radix = args.size() > 1 ? asIntCast(asNumber(args[1])) : 0;
            return Value::num(asParseInt(v.toString(), radix));
        }
        if (name == "parseFloat") return Value::num(asParseFloat(v.toString()));
        if (name == "int") return Value::intg(asIntCast(asNumber(v)));
        if (name == "uint") return Value::uintv(static_cast<uint32_t>(asIntCast(asNumber(v))));
        if (name == "Number") return Value::num(asNumber(v));
        if (name == "String") return Value::str(v.toString());
        if (name == "Boolean") return Value::boolean(v.toBool());
        if (name == "isNaN") return Value::boolean(std::isnan(asNumber(v)));
        if (name == "isFinite") return Value::boolean(std::isfinite(asNumber(v)));
        if (name == "encodeURI") return Value::str(uriEncode(v.toString(), false));
        if (name == "encodeURIComponent") return Value::str(uriEncode(v.toString(), true));
        if (name == "decodeURI" || name == "decodeURIComponent" ||
            name == "escape" || name == "unescape") {
            if (name == "escape") return Value::str(uriEncode(v.toString(), false));
            return Value::str(uriDecode(v.toString()));
        }
        if (name == "getQualifiedClassName") {
            if (v.kind == VKind::Object && v.obj) return Value::str(v.obj->className);
            switch (v.kind) {
                case VKind::Null: return Value::str("null");
                case VKind::Undefined: return Value::str("void");
                case VKind::Bool: return Value::str("Boolean");
                case VKind::Int: return Value::str("int");
                case VKind::UInt: return Value::str("uint");
                case VKind::Number: return Value::str("Number");
                case VKind::String: return Value::str("String");
                default: break;
            }
            return Value::str("Object");
        }
        if (name == "getQualifiedSuperclassName") {
            if (v.kind == VKind::Object && v.obj && v.obj->cls && v.obj->cls->superClass)
                return Value::str(v.obj->cls->superClass->name);
            return Value::nullv();
        }
        if (name == "describeType") {
            const std::string cn = (v.kind == VKind::Object && v.obj) ? v.obj->className : std::string("Object");
            std::string base = "Object";
            if (v.kind == VKind::Object && v.obj && v.obj->cls && v.obj->cls->superClass)
                base = v.obj->cls->superClass->name;
            // AS3 返回 flash.xml.XML 对象（而不是字符串）；Value::toString()
            // 会把 XML 对象序列化回 XML 文本，故旧代码按字符串使用仍然成立。
            return xmlParseToValue(*this,
                "<type name=\"" + xmlEscapeString(cn, true) + "\" base=\"" +
                xmlEscapeString(base, true) +
                "\" isDynamic=\"true\" isFinal=\"false\" isStatic=\"false\">"
                "<extendsClass type=\"" + xmlEscapeString(base, true) + "\"/>"
                "<extendsClass type=\"Object\"/></type>");
        }
    }
    // flash.utils.getDefinitionByName：Flash 动态创建库元件的标准路径，
    // new (getDefinitionByName("Xxx") as Class)() 依赖它返回可用的类对象。
    if (globalCall && name == "getDefinitionByName" && !args.empty()) {
        const std::string q = args[0].toString();
        auto it = globals_->props.find(q);
        if (it == globals_->props.end()) {
            const size_t p = q.rfind('.');
            if (p != std::string::npos) it = globals_->props.find(q.substr(p + 1));
        }
        if (it != globals_->props.end() && it->second.kind == VKind::Object &&
            it->second.obj && it->second.obj->kind == ObjKind::Class)
            return it->second;
        for (const auto& o : objects_) {
            if (!o || o->kind != ObjKind::Class) continue;
            const std::size_t tail = o->className.size() - q.size();
            if (o->className == q ||
                (o->className.size() > q.size() && o->className[tail - 1] == '.' &&
                 o->className.compare(tail, q.size(), q) == 0))
                return Value::object(o);
        }
        if (std::getenv("SWF_AS_MISS"))
            std::fprintf(stderr, "[miss] getDefinitionByName '%s'\n", q.c_str());
        return Value::undefined();
    }
    // flash.net.navigateToURL：无头环境没有外部导航；静默接受请求而不是留下
    // 未解析调用，后续脚本（例如关掉窗口前的数据保存）才能继续正常执行。
    if (globalCall && name == "navigateToURL") return Value::undefined();
    // 类对象作为可调用值：AS3 中 ClassName(value) 等价于类型转换（如 MovieClip(parent)、Class(o)）。
    if (obj->kind == ObjKind::Class && obj->classDef && args.size() == 1) {
        size_t dot = obj->classDef->name.rfind('.');
        std::string sn = (dot == std::string::npos) ? obj->classDef->name : obj->classDef->name.substr(dot + 1);
        if (name == obj->classDef->name || name == sn) {
            const Value& v = args[0];
            if (v.isNullOrUndef()) return Value::nullv();
            return v;
        }
    }
    // 1) 原生类方法
    if (obj->classDef) {
        for (const auto& m : obj->classDef->instanceMembers)
            if (m.name == name && m.native && m.kind == Member::Method)
                return m.native(obj, args);
        for (const auto& m : obj->classDef->staticMembers)
            if (m.name == name && m.native && m.kind == Member::Method)
                return m.native(obj, args);
    }
    // 1b) 类对象（ObjKind::Class）的静态成员方法（含父类），供 Foo.bar() 直接调用
    if (obj->classDef) {
        for (auto c = obj->classDef; c; c = c->superClass) {
            for (const auto& m : c->staticMembers) {
                if (m.name != name || m.kind != Member::Method) continue;
                if (std::getenv("SWF_AS_DEBUG") && (name == "frameHandler" || name == "getFrameIndexByLabel"))
                    std::fprintf(stderr, "[as] %s mi=%u body=%d\n", name.c_str(), m.methodIndex,
                                 (m.methodIndex < abc_.methods.size()) ? abc_.methods[m.methodIndex].bodyIndex : -1);
                if (m.native) {
                    Value r = m.native(obj, args);
                    if (std::getenv("SWF_AS_DEBUG") && name == "getFrameIndexByLabel")
                        std::fprintf(stderr, "[fu-ret] %s\n", r.toString().c_str());
                    return r;
                }
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size())) {
                        Value r = exec(abc_.methodBodies[bi], args, obj);
                        if (std::getenv("SWF_AS_DEBUG") && name == "getFrameIndexByLabel") {
                            std::string tcn = "?"; int tcid = -1;
                            if (args.size() > 0 && args[0].obj) {
                                tcn = args[0].obj->className;
                                if (args[0].obj->display) tcid = args[0].obj->display->characterId;
                            }
                            std::fprintf(stderr, "[fu-ret] target=%s cid=%d ret=%s\n",
                                         tcn.c_str(), tcid, r.toString().c_str());
                        }
                        return r;
                    }
                }
            }
            for (const auto& m : c->instanceMembers) {
                if (m.name != name || m.kind != Member::Method) continue;
                if (m.native) return m.native(obj, args);
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                        return exec(abc_.methodBodies[bi], args, obj);
                }
            }
        }
    }
    // 2) 类成员方法（原生优先，其次字节码）——沿父类链，可命中原生基类成员
    for (auto c = obj->cls; c; c = c->superClass) {
        for (const auto& m : c->instanceMembers)
            if (m.name == name && m.kind == Member::Method) {
                if (std::getenv("SWF_AS_CALLS"))
                    std::fprintf(stderr, "[call] %s.%s mi=%u body=%d argc=%zu\n", obj->className.c_str(),
                                 name.c_str(), m.methodIndex,
                                 (m.methodIndex < abc_.methods.size()) ? abc_.methods[m.methodIndex].bodyIndex : -1,
                                 args.size());
                if (m.native) return m.native(obj, args);
                if (m.methodIndex < abc_.methods.size()) {
                    int bi = abc_.methods[m.methodIndex].bodyIndex;
                    if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                        return exec(abc_.methodBodies[bi], args, obj);
                }
            }
    }
    // 3) 动态属性中的可调用值
    auto dp = obj->props.find(name);
    if (dp != obj->props.end() && dp->second.kind == VKind::Object && dp->second.obj) {
        auto fn = dp->second.obj;
        // 属性值为类对象：ClassName(value) 等价于类型转换（MovieClip(parent) 等）
        if (fn->kind == ObjKind::Class && fn->classDef && args.size() == 1) {
            const Value& v = args[0];
            return v.isNullOrUndef() ? Value::nullv() : v;
        }
        auto mi = fn->props.find("$method");
        if (mi != fn->props.end()) {
            uint32_t midx = mi->second.u;
            if (midx < abc_.methods.size()) {
                int bi = abc_.methods[midx].bodyIndex;
                if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                    return exec(abc_.methodBodies[bi], args, obj);
            }
        }
        if (fn->props.count("$native")) return invokeValue(dp->second, args, obj);
    }
    // 4) 时间轴 / objectForDisplay 生成的包装对象、以及 AS3 子类实例通常没有原生
    // classDef。按 EventDispatcher / 显示对象继承链回退到原生类成员，保证
    // addEventListener / addChild / gotoAndStop 等可用（仅在上述查找全部落空后）。
    if (!obj->classDef) {
        static const char* kNativeDisplayClasses[] = {
            "XML", "XMLList", "ByteArray",
            "EventDispatcher", "DisplayObject", "Sprite", "MovieClip",
            "InteractiveObject", "TextField", "Stage", "ContextMenu",
            "Array", "String", "Object"};
        // 先按对象自身的类名找（XML 与 XMLList 同名方法语义不同，不能混用），
        // 找不到再按显示对象继承链依次回退。
        std::vector<std::string> candidates;
        if (!obj->className.empty()) candidates.push_back(obj->className);
        for (const char* cn : kNativeDisplayClasses) candidates.emplace_back(cn);
        for (const auto& cn : candidates) {
            const auto it = globals_->props.find(cn);
            if (it == globals_->props.end() || it->second.kind != VKind::Object ||
                !it->second.obj || !it->second.obj->classDef) continue;
            for (auto cd = it->second.obj->classDef; cd; cd = cd->superClass) {
                for (const auto& m : cd->instanceMembers) {
                    if (m.name == name && m.native && m.kind == Member::Method)
                        return m.native(obj, args);
                }
            }
        }
    }
    if (std::getenv("SWF_AS_MISS"))
        std::fprintf(stderr, "[miss] call %s.%s argc=%zu cls=%s\n",
                     obj->className.c_str(), name.c_str(), args.size(),
                     obj->cls ? obj->cls->name.c_str() : "-");
    if (std::getenv("SWF_AS_DEBUG") && name == "addChild")
        std::fprintf(stderr, "[as] addChild NOT-DISPATCHED recv=%s cls=%s classDef=%s\n",
                     obj->className.c_str(), obj->cls ? obj->cls->name.c_str() : "-",
                     obj->classDef ? obj->classDef->name.c_str() : "-");
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
        Member shift; shift.kind = Member::Method; shift.name = "shift";
        shift.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::undefined();
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            if (n <= 0) return Value::undefined();
            Value r = Value::undefined();
            auto it0 = self->props.find("0");
            if (it0 != self->props.end()) { r = it0->second; self->props.erase(it0); }
            for (int i = 1; i < n; ++i) {
                auto it = self->props.find(std::to_string(i));
                if (it != self->props.end()) self->props[std::to_string(i - 1)] = it->second;
                else self->props.erase(std::to_string(i - 1));
            }
            self->props.erase(std::to_string(n - 1));
            self->props["$length"] = Value::intg(n - 1);
            return r;
        };
        c->classDef->instanceMembers.push_back(shift);
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
        Member sp; sp.kind = Member::Method; sp.name = "splice";
        sp.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto out = std::make_shared<Object>();
            out->kind = ObjKind::Array;
            out->className = "Array";
            out->props["$length"] = Value::intg(0);
            if (!self) return Value::object(out);
            int n = self->props.count("$length") ? self->props["$length"].toInt() : 0;
            int start = a.empty() ? 0 : a[0].toInt();
            if (start < 0) start = std::max(0, n + start);
            if (start > n) start = n;
            int del = (a.size() > 1) ? a[1].toInt() : n - start;
            del = std::max(0, std::min(del, n - start));
            std::vector<Value> tail;
            for (int i = start + del; i < n; ++i) {
                auto it = self->props.find(std::to_string(i));
                tail.push_back(it != self->props.end() ? it->second : Value::undefined());
            }
            for (int i = 0; i < del; ++i) {
                auto it = self->props.find(std::to_string(start + i));
                out->props[std::to_string(i)] = it != self->props.end() ? it->second : Value::undefined();
                self->props.erase(std::to_string(start + i));
            }
            out->props["$length"] = Value::intg(del);
            std::vector<Value> ins;
            for (std::size_t i = 2; i < a.size(); ++i) ins.push_back(a[i]);
            int w = start;
            for (const auto& v : ins) self->props[std::to_string(w++)] = v;
            for (const auto& v : tail) self->props[std::to_string(w++)] = v;
            self->props["$length"] = Value::intg(w);
            return Value::object(out);
        };
        c->classDef->instanceMembers.push_back(sp);
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
        Member ca; ca.kind = Member::Method; ca.name = "charAt";
        ca.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            int i = a.empty() ? 0 : a[0].toInt();
            if (i < 0 || i >= static_cast<int>(s.size())) return Value::str("");
            return Value::str(std::string(1, s[static_cast<std::size_t>(i)]));
        };
        c->classDef->instanceMembers.push_back(ca);
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
        // FIX-2026-020：String.replace 缺失——换装关的 onListClick1 用
        // int(e.target.name.replace('a','')) 取物品序号，replace 不存在时表达式
        // 得 undefined → gotoAndStop(0) 把 LV 部件重置回底帧，点击任何装饰都
        // 表现为「没反应」。AS3 语义：pattern 为字符串时只替换第一处。
        Member rp; rp.kind = Member::Method; rp.name = "replace";
        rp.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            auto p = self->props.find("$str");
            std::string s = (p != self->props.end()) ? p->second.s : std::string();
            if (a.size() < 2) return Value::str(s);
            std::string pat = a[0].toString();
            if (pat.empty()) return Value::str(s);
            std::string rep = a[1].toString();
            auto pos = s.find(pat);
            if (pos == std::string::npos) return Value::str(s);
            return Value::str(s.substr(0, pos) + rep + s.substr(pos + pat.size()));
        };
        c->classDef->instanceMembers.push_back(rp);
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
        // Math.random 用固定种子保证逐帧可复现；SWF_AS_SEED 可显式指定种子。
        addMath("random", 0, [](std::shared_ptr<Object>, const std::vector<Value>&) {
            static const bool seeded = [] {
                const char* s = std::getenv("SWF_AS_SEED");
                std::srand(s ? static_cast<unsigned>(std::strtoul(s, nullptr, 10)) : 1u);
                return true;
            }();
            (void)seeded;
            return Value::num(static_cast<double>(std::rand()) / RAND_MAX); });
        addMath("max", 0, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            if (a.empty()) return Value::num(-INFINITY);
            double r = asNumber(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) r = std::fmax(r, asNumber(a[i]));
            return Value::num(r); });
        addMath("min", 0, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            if (a.empty()) return Value::num(INFINITY);
            double r = asNumber(a[0]);
            for (std::size_t i = 1; i < a.size(); ++i) r = std::fmin(r, asNumber(a[i]));
            return Value::num(r); });
        for (const auto& mf : {std::make_pair("sin",  (double(*)(double))&std::sin),
                               std::make_pair("cos",  (double(*)(double))&std::cos),
                               std::make_pair("tan",  (double(*)(double))&std::tan),
                               std::make_pair("asin", (double(*)(double))&std::asin),
                               std::make_pair("acos", (double(*)(double))&std::acos),
                               std::make_pair("atan", (double(*)(double))&std::atan),
                               std::make_pair("log",  (double(*)(double))&std::log),
                               std::make_pair("exp",  (double(*)(double))&std::exp)}) {
            addMath(mf.first, 1, [f = mf.second](std::shared_ptr<Object>,
                                                 const std::vector<Value>& a) {
                return Value::num(a.empty() ? 0.0 : f(asNumber(a[0]))); });
        }
        addMath("atan2", 2, [](std::shared_ptr<Object>, const std::vector<Value>& a) {
            return Value::num(a.size() < 2 ? 0.0 : std::atan2(asNumber(a[0]), asNumber(a[1]))); });
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
        c->classDef->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            double ms = 0;
            if (a.empty()) {
                ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            } else if (a.size() == 1) {
                ms = asNumber(a[0]);
            } else {
                std::tm t{};
                t.tm_year = a[0].toInt() - 1900;
                t.tm_mon = a.size() > 1 ? a[1].toInt() : 0;
                t.tm_mday = a.size() > 2 ? a[2].toInt() : 1;
                t.tm_hour = a.size() > 3 ? a[3].toInt() : 0;
                t.tm_min = a.size() > 4 ? a[4].toInt() : 0;
                t.tm_sec = a.size() > 5 ? a[5].toInt() : 0;
                t.tm_isdst = -1;
                ms = static_cast<double>(std::mktime(&t)) * 1000.0 +
                     (a.size() > 6 ? a[6].toNumber() : 0.0);
            }
            if (self) self->props["$time"] = Value::num(ms);
            return Value::undefined();
        };
        auto add = [&](const char* n, NativeFn fn) {
            Member m; m.kind = Member::Method; m.name = n; m.native = std::move(fn);
            c->classDef->instanceMembers.push_back(m);
        };
        add("getTime", [](std::shared_ptr<Object> self, const std::vector<Value>&) {
            return Value::num(dateTimeOf(self)); });
        add("valueOf", [](std::shared_ptr<Object> self, const std::vector<Value>&) {
            return Value::num(dateTimeOf(self)); });
        add("setTime", [](std::shared_ptr<Object> self, const std::vector<Value>& a) {
            const double ms = a.empty() ? 0.0 : asNumber(a[0]);
            if (self) self->props["$time"] = Value::num(ms);
            return Value::num(ms); });
        struct DateGetter { const char* name; int which; bool local; };
        const DateGetter getters[] = {
            {"getFullYear", 0, true},   {"getMonth", 1, true},
            {"getDate", 2, true},       {"getDay", 3, true},
            {"getHours", 4, true},      {"getMinutes", 5, true},
            {"getSeconds", 6, true},
            {"getUTCFullYear", 0, false}, {"getUTCMonth", 1, false},
            {"getUTCDate", 2, false},     {"getUTCDay", 3, false},
            {"getUTCHours", 4, false},    {"getUTCMinutes", 5, false},
            {"getUTCSeconds", 6, false},
        };
        for (const auto& g : getters) {
            add(g.name, [w = g.which, loc = g.local](std::shared_ptr<Object> self,
                                                      const std::vector<Value>&) {
                return Value::intg(dateField(dateTm(dateTimeOf(self), loc), w)); });
        }
        add("getMilliseconds", [](std::shared_ptr<Object> self, const std::vector<Value>&) {
            const double ms = dateTimeOf(self);
            return Value::intg(static_cast<int32_t>(std::fmod(std::fmod(ms, 1000.0) + 1000.0, 1000.0))); });
        add("getTimezoneOffset", [](std::shared_ptr<Object> self, const std::vector<Value>&) {
            const std::tm lt = dateTm(dateTimeOf(self), true);
            const std::tm gt = dateTm(dateTimeOf(self), false);
            const int diff = (lt.tm_yday - gt.tm_yday) * 1440 +
                             (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
            return Value::num(-static_cast<double>(diff)); });
        add("getUTCMilliseconds", [](std::shared_ptr<Object> self, const std::vector<Value>&) {
            const double ms = dateTimeOf(self);
            return Value::intg(static_cast<int32_t>(std::fmod(std::fmod(ms, 1000.0) + 1000.0, 1000.0))); });
        // which: 0=年 1=月 2=日 4=时 5=分 6=秒 7=毫秒
        struct DateSetter { const char* name; int which; bool local; };
        const DateSetter setters[] = {
            {"setFullYear", 0, true}, {"setMonth", 1, true}, {"setDate", 2, true},
            {"setHours", 4, true}, {"setMinutes", 5, true}, {"setSeconds", 6, true},
            {"setMilliseconds", 7, true},
            {"setUTCFullYear", 0, false}, {"setUTCMonth", 1, false}, {"setUTCDate", 2, false},
            {"setUTCHours", 4, false}, {"setUTCMinutes", 5, false}, {"setUTCSeconds", 6, false},
            {"setUTCMilliseconds", 7, false},
        };
        for (const auto& s : setters) {
            add(s.name, [w = s.which, loc = s.local](std::shared_ptr<Object> self,
                                                     const std::vector<Value>& a) -> Value {
                const double ms = dateTimeOf(self);
                const double arg = a.empty() ? 0.0 : asNumber(a[0]);
                const double millis = std::fmod(std::fmod(ms, 1000.0) + 1000.0, 1000.0);
                double out;
                if (w == 7) {
                    out = std::floor(ms / 1000.0) * 1000.0 + arg;
                } else {
                    std::tm t = dateTm(ms, loc);
                    switch (w) {
                        case 0: t.tm_year = static_cast<int>(arg) - 1900; break;
                        case 1: t.tm_mon = static_cast<int>(arg); break;
                        case 2: t.tm_mday = static_cast<int>(arg); break;
                        case 4: t.tm_hour = static_cast<int>(arg); break;
                        case 5: t.tm_min = static_cast<int>(arg); break;
                        case 6: t.tm_sec = static_cast<int>(arg); break;
                        default: break;
                    }
                    out = dateAssemble(t, loc, millis);
                }
                if (self) self->props["$time"] = Value::num(out);
                return Value::num(out); });
        }
        struct DateFmt { const char* name; int which; };
        const DateFmt fmts[] = {
            {"toString", 0}, {"toLocaleString", 0}, {"toUTCString", 1}, {"toGMTString", 1},
            {"toISOString", 2}, {"toJSON", 2}, {"toDateString", 3}, {"toLocaleDateString", 3},
            {"toTimeString", 4}, {"toLocaleTimeString", 4},
        };
        for (const auto& f : fmts) {
            add(f.name, [w = f.which](std::shared_ptr<Object> self,
                                      const std::vector<Value>&) -> Value {
                return Value::str(dateFormat(dateTimeOf(self), w)); });
        }
    }

    // ---- JSON ----
    {
        auto c = mkClass("JSON", ObjKind::Generic);
        Member st; st.kind = Member::Method; st.name = "stringify"; st.isStatic = true;
        st.native = [](std::shared_ptr<Object>, const std::vector<Value>& a) -> Value {
            if (a.empty()) return Value::undefined();
            const std::string out = jsonStringify(a[0], 0);
            return out.empty() ? Value::undefined() : Value::str(out);
        };
        c->classDef->staticMembers.push_back(st);
        Member ps; ps.kind = Member::Method; ps.name = "parse"; ps.isStatic = true;
        ps.native = [this](std::shared_ptr<Object>, const std::vector<Value>& a) -> Value {
            if (a.empty() || a[0].kind != VKind::String) return Value::undefined();
            std::size_t p = 0;
            Value v;
            if (!jsonParseValue(*this, a[0].s, p, v, 0)) return Value::undefined();
            jsonSkipWs(a[0].s, p);
            if (p != a[0].s.size()) return Value::undefined();
            if (a.size() > 1 && jsonIsCallable(a[1]))
                return jsonRevive(*this, a[1], "", v, 0);
            return v;
        };
        c->classDef->staticMembers.push_back(ps);
    }

    // ---- ContextMenu（右键菜单；游戏常用 hideBuiltInItems 隐藏内置项）----
    {
        auto c = mkClass("ContextMenu", ObjKind::Generic);
        globals_->props["flash.ui.ContextMenu"] = Value::object(c);
        c->classDef->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (self) {
                auto a = std::make_shared<Object>();
                a->kind = ObjKind::Array;
                a->className = "Array";
                a->props["$length"] = Value::intg(0);
                self->props["customItems"] = Value::object(a);
            }
            return Value::undefined();
        };
        Member h; h.kind = Member::Method; h.name = "hideBuiltInItems";
        h.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (self) {
                auto bi = std::make_shared<Object>();
                bi->className = "ContextMenuBuiltInItems";
                for (const char* k : {"save", "zoom", "quality", "play", "loop", "rewind",
                                      "forwardAndBack", "print", "showAll", "normalView"})
                    bi->props[k] = Value::boolean(false);
                self->props["builtInItems"] = Value::object(bi);
            }
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(h);
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
        // stage.loaderInfo：与 DisplayObject.loaderInfo 同语义（本地整片加载），
        // preloader 脚本普遍经 stage.loaderInfo 取进度。
        Member sli; sli.kind = Member::Getter; sli.name = "loaderInfo";
        sli.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::nullv();
            auto& cache = self->props["loaderInfo"];
            if (cache.kind == VKind::Object && cache.obj) return cache;
            auto info = newObject(ObjKind::Generic, "LoaderInfo");
            info->props["bytesLoaded"] = Value::intg(100);
            info->props["bytesTotal"] = Value::intg(100);
            info->props["url"] = Value::str("file://local.swf");
            if (self->display)
                info->props["content"] = Value::object(self);
            info->props["contentLoaderInfo"] = Value::object(info);
            loaderInfos_.push_back(info);
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] stage.loaderInfo accessed\n");
            cache = Value::object(info);
            return cache;
        };
        c->classDef->instanceMembers.push_back(sli);
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
        for (const char* pn : {"target", "currentTarget", "bubbles", "cancelable"}) {
            Member g; g.kind = Member::Getter; g.name = pn;
            g.native = [pn](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                auto p = self->props.find(pn);
                return p != self->props.end() ? p->second : Value::nullv();
            };
            c->classDef->instanceMembers.push_back(g);
        }
        // new Event(type, bubbles=false, cancelable=false)：完整初始化事件流字段。
        c->classDef->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            initEventProps(self, a.empty() ? Value::str("") : a[0],
                           a.size() > 1 && a[1].toBool(), a.size() > 2 && a[2].toBool());
            return Value::undefined();
        };
        Member ph; ph.kind = Member::Getter; ph.name = "eventPhase";
        ph.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto p = self->props.find("eventPhase");
            return p != self->props.end() ? p->second : Value::intg(2);
        };
        c->classDef->instanceMembers.push_back(ph);
        for (const char* mn : {"stopPropagation", "stopImmediatePropagation",
                              "preventDefault", "isDefaultPrevented", "clone"}) {
            Member m; m.kind = Member::Method; m.name = mn;
            m.native = [mn](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self) return Value::undefined();
                const std::string n = mn;
                if (n == "stopPropagation") self->props["$stop"] = Value::boolean(true);
                else if (n == "stopImmediatePropagation") {
                    self->props["$stop"] = Value::boolean(true);
                    self->props["$stopImmediate"] = Value::boolean(true);
                } else if (n == "preventDefault") {
                    auto c = self->props.find("cancelable");
                    if (c != self->props.end() && c->second.toBool())
                        self->props["$defaultPrevented"] = Value::boolean(true);
                } else if (n == "isDefaultPrevented") {
                    auto p = self->props.find("$defaultPrevented");
                    return Value::boolean(p != self->props.end() && p->second.toBool());
                } else if (n == "clone") {
                    auto o = std::make_shared<Object>();
                    o->kind = ObjKind::Event;
                    o->className = self->className;
                    o->cls = self->cls;
                    o->props = self->props;
                    o->props["target"] = Value::nullv();
                    o->props["currentTarget"] = Value::nullv();
                    o->props["eventPhase"] = Value::intg(2);
                    o->props["$stop"] = Value::boolean(false);
                    o->props["$stopImmediate"] = Value::boolean(false);
                    return Value::object(o);
                }
                return Value::undefined();
            };
            c->classDef->instanceMembers.push_back(m);
        }
        for (const auto& pc : {std::make_pair("CAPTURING_PHASE", 1),
                               std::make_pair("AT_TARGET", 2),
                               std::make_pair("BUBBLING_PHASE", 3)}) {
            Member m; m.kind = Member::Const; m.name = pc.first;
            m.isStatic = true; m.defaultValue = Value::intg(pc.second);
            c->classDef->staticMembers.push_back(m);
        }
        struct { const char* name; const char* val; } consts[] = {
            {"ENTER_FRAME", "enterFrame"}, {"FRAME_CONSTRUCTED", "frameConstructed"},
            {"EXIT_FRAME", "exitFrame"}, {"ADDED_TO_STAGE", "addedToStage"},
            {"REMOVED_FROM_STAGE", "removedFromStage"}, {"ADDED", "added"},
            {"REMOVED", "removed"}, {"COMPLETE", "complete"}, {"CHANGE", "change"},
            {"ACTIVATE", "activate"}, {"DEACTIVATE", "deactivate"}, {"CANCEL", "cancel"},
            {"CLOSE", "close"}, {"CONNECT", "connect"}, {"INIT", "init"},
            {"OPEN", "open"}, {"RESIZE", "resize"}, {"SELECT", "select"},
            {"UNLOAD", "unload"}, {"TIMER", "timer"}, {"TIMER_COMPLETE", "timerComplete"},
            {"SCROLL", "scroll"}, {"SOUND_COMPLETE", "soundComplete"}, {"RENDER", "render"},
            {"MOUSE_LEAVE", "mouseLeave"},
        };
        for (const auto& k : consts) {
            Member en; en.kind = Member::Method; en.name = k.name; en.isStatic = true;
            en.defaultValue = Value::str(k.val);
            c->classDef->staticMembers.push_back(en);
        }
    }

    // ---- TimerEvent：Timer 派发的事件类型常量与 type/target getter ----
    {
        auto c = mkClass("TimerEvent", ObjKind::Event);
        c->classDef->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self) {
                self->props["type"] = a.empty() ? Value::str("timer") : a[0];
                if (a.size() > 1) self->props["bubbles"] = a[1];
                if (a.size() > 2) self->props["cancelable"] = a[2];
            }
            return Value::undefined();
        };
        for (const auto& k : {std::make_pair("TIMER", "timer"),
                              std::make_pair("TIMER_COMPLETE", "timerComplete")}) {
            Member en; en.kind = Member::Method; en.name = k.first; en.isStatic = true;
            en.defaultValue = Value::str(k.second);
            c->classDef->staticMembers.push_back(en);
        }
        for (const char* n : {"type", "target", "currentTarget", "bubbles", "cancelable"}) {
            Member g; g.kind = Member::Getter; g.name = n;
            g.native = [n](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self) return Value::nullv();
                auto p = self->props.find(n);
                return p == self->props.end() ? Value::nullv() : p->second;
            };
            c->classDef->instanceMembers.push_back(g);
        }
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
        const bool isByteArray = std::strcmp(nm, "ByteArray") == 0;
        const bool isXml = std::strcmp(nm, "XML") == 0 || std::strcmp(nm, "XMLList") == 0;
        // 通用：toString（XML/XMLList 用真实序列化实现，稍后单独挂接）
        if (!isXml) {
            Member ts; ts.kind = Member::Method; ts.name = "toString";
            ts.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                std::string cn = self && !self->className.empty() ? self->className : std::string("Object");
                return Value::str("[object " + cn + "]");
            };
            cd->instanceMembers.push_back(ts);
        }
        // 常用无副作用方法；ByteArray 的字节读写方法留给用户可见的真实实现。
        for (const char* mn : {"start", "stop", "play", "close", "load", "reset",
                               "writeByte", "writeBytes", "readByte", "readBytes",
                               "getBytesTotal", "getBytesLoaded", "clear", "flush",
                               "setProperty", "getProperty", "clone", "concat"}) {
            if (isByteArray &&
                (std::strcmp(mn, "writeByte") == 0 || std::strcmp(mn, "writeBytes") == 0 ||
                 std::strcmp(mn, "readByte") == 0 || std::strcmp(mn, "readBytes") == 0 ||
                 std::strcmp(mn, "getBytesTotal") == 0 || std::strcmp(mn, "getBytesLoaded") == 0 ||
                 std::strcmp(mn, "clear") == 0 || std::strcmp(mn, "flush") == 0))
                continue;
            Member m; m.kind = Member::Method; m.name = mn;
            m.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
                return Value::undefined();
            };
            cd->instanceMembers.push_back(m);
        }
        if (std::string(nm) == "MouseEvent" || std::string(nm) == "KeyboardEvent") {
            // new MouseEvent(type, bubbles, cancelable, localX, localY, ...)
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
                if (!self) return Value::undefined();
                if (!a.empty()) self->props["type"] = a[0];
                if (a.size() > 3) self->props["localX"] = a[3];
                if (a.size() > 4) self->props["localY"] = a[4];
                return Value::undefined();
            };
            struct { const char* name; const char* val; } mconsts[] = {
                {"CLICK", "click"}, {"DOUBLE_CLICK", "doubleClick"},
                {"MOUSE_DOWN", "mouseDown"}, {"MOUSE_UP", "mouseUp"},
                {"MOUSE_MOVE", "mouseMove"}, {"MOUSE_OVER", "mouseOver"},
                {"MOUSE_OUT", "mouseOut"}, {"ROLL_OVER", "rollOver"},
                {"ROLL_OUT", "rollOut"}, {"MOUSE_WHEEL", "mouseWheel"},
                {"KEY_DOWN", "keyDown"}, {"KEY_UP", "keyUp"},
            };
            for (const auto& k : mconsts) {
                Member en; en.kind = Member::Method; en.name = k.name; en.isStatic = true;
                en.defaultValue = Value::str(k.val);
                cd->staticMembers.push_back(en);
            }
        }
        // IOErrorEvent / SecurityErrorEvent：静态常量常被 preloader 用于注册网络
        // 失败监听；缺省时监听键解析为 undefined，失败路径静默断裂。
        if (std::string(nm) == "IOErrorEvent") {
            Member en; en.kind = Member::Method; en.name = "IO_ERROR"; en.isStatic = true;
            en.defaultValue = Value::str("ioError");
            cd->staticMembers.push_back(en);
        }
        if (std::string(nm) == "SecurityErrorEvent") {
            Member en; en.kind = Member::Method; en.name = "SECURITY_ERROR"; en.isStatic = true;
            en.defaultValue = Value::str("securityError");
            cd->staticMembers.push_back(en);
        }
    }

    // ---- flash.system.Security（AVM2 内建类）----
    // 运行时没有 URL 来源信息，按"具备网络加载能力"统一报告 remote 沙箱：
    // URLLoader.load() 会派发 COMPLETE，SWF 的网络分支因而可正常执行。
    // allowDomain / allowInsecureDomain 在本地沙箱下为 no-op。
    {
        auto c = mkClass("Security", ObjKind::Generic);
        globals_->props["flash.system.Security"] = Value::object(c);
        auto cd = c->classDef;
        Member st; st.kind = Member::Method; st.name = "sandboxType"; st.isStatic = true;
        st.defaultValue = Value::str("remote");
        cd->staticMembers.push_back(st);
        Member ad; ad.kind = Member::Method; ad.name = "allowDomain"; ad.isStatic = true;
        ad.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::undefined();   // 本地沙箱：允许即 no-op
        };
        cd->staticMembers.push_back(ad);
        Member ad2; ad2.kind = Member::Method; ad2.name = "allowInsecureDomain"; ad2.isStatic = true;
        ad2.native = ad.native;
        cd->staticMembers.push_back(ad2);
        Member ld; ld.kind = Member::Method; ld.name = "loadPolicyFile"; ld.isStatic = true;
        ld.native = ad.native;
        cd->staticMembers.push_back(ld);
    }

    // ---- 内建数据 / XML / 补间 / 加载 / 媒体：把占位换成真实运行时语义 ----
    {
        auto classObj = [&](const char* n) -> std::shared_ptr<Object> {
            auto it = globals_->props.find(n);
            return (it != globals_->props.end() && it->second.kind == VKind::Object)
                ? it->second.obj : nullptr;
        };
        auto classDefOf = [&](const char* n) -> std::shared_ptr<ClassDef> {
            auto o = classObj(n);
            return o ? o->classDef : nullptr;
        };
        // 同名成员就地替换：通用占位（load/play/...）不能挡住后面的真实实现，
        // 因为 callMethod / getPropertyImpl 都只取成员链上的第一个匹配。
        auto addMethod = [](const std::shared_ptr<ClassDef>& cd, const char* n,
                            NativeFn fn, bool stat = false) {
            if (!cd) return;
            auto& dst = stat ? cd->staticMembers : cd->instanceMembers;
            for (auto& m : dst) {
                if (m.name != n) continue;
                m.kind = Member::Method; m.isStatic = stat; m.native = std::move(fn);
                return;
            }
            Member m; m.kind = Member::Method; m.name = n; m.isStatic = stat;
            m.native = std::move(fn);
            dst.push_back(m);
        };
        auto noop = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            return Value::undefined();
        };

        // ---- ByteArray：小端字节缓冲，与 applicationDomain.domainMemory 共享 ----
        if (auto cd = classDefOf("ByteArray")) {
            globals_->props["flash.utils.ByteArray"] = Value::object(classObj("ByteArray"));
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self) return Value::undefined();
                self->bytes = std::make_shared<std::vector<std::uint8_t>>();
                self->props["position"] = Value::intg(0);
                self->props["endian"] = Value::str("littleEndian");
                return Value::undefined();
            };
            auto getter = [&](const char* n, NativeFn fn) {
                Member m; m.kind = Member::Getter; m.name = n; m.native = std::move(fn);
                cd->instanceMembers.push_back(m);
            };
            getter("length", [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                return Value::intg(static_cast<int32_t>(baBytes(self).size())); });
            getter("bytesAvailable", [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                const std::size_t n = baBytes(self).size(), p = baPos(self);
                return Value::intg(static_cast<int32_t>(n > p ? n - p : 0)); });
            struct BinMethod { const char* name; int width; bool sign, isFloat, write; };
            const BinMethod bms[] = {
                {"readByte", 1, true, false, false},
                {"readUnsignedByte", 1, false, false, false},
                {"readShort", 2, true, false, false},
                {"readUnsignedShort", 2, false, false, false},
                {"readInt", 4, true, false, false},
                {"readUnsignedInt", 4, false, false, false},
                {"readFloat", 4, false, true, false},
                {"readDouble", 8, false, true, false},
                {"writeByte", 1, true, false, true},
                {"writeShort", 2, true, false, true},
                {"writeInt", 4, true, false, true},
                {"writeUnsignedInt", 4, false, false, true},
                {"writeFloat", 4, false, true, true},
                {"writeDouble", 8, false, true, true},
            };
            for (const auto& bm : bms) {
                addMethod(cd, bm.name,
                    [w = bm.width, sg = bm.sign, fl = bm.isFloat, wr = bm.write]
                    (std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
                        if (wr) {
                            const double v = a.empty() ? 0.0 : a[0].toNumber();
                            if (fl) baWriteFloat(self, w == 8, v);
                            else baWriteInt(self, w, v);
                            return Value::undefined();
                        }
                        if (fl) return baReadFloat(self, w == 8);
                        return baReadInt(self, w, sg);
                    });
            }
            addMethod(cd, "readBoolean", [](std::shared_ptr<Object> self,
                                            const std::vector<Value>&) -> Value {
                const Value v = baReadInt(self, 1, false);
                return Value::boolean(!v.isUndefined() && v.toBool()); });
            addMethod(cd, "writeBoolean", [](std::shared_ptr<Object> self,
                                             const std::vector<Value>& a) -> Value {
                baWriteInt(self, 1, (a.empty() || !a[0].toBool()) ? 0.0 : 1.0);
                return Value::undefined(); });
            addMethod(cd, "clear", [](std::shared_ptr<Object> self,
                                      const std::vector<Value>&) -> Value {
                baBytes(self).clear(); baSetPos(self, 0); return Value::undefined(); });
            addMethod(cd, "getBytesTotal", [](std::shared_ptr<Object> self,
                                              const std::vector<Value>&) -> Value {
                return Value::intg(static_cast<int32_t>(baBytes(self).size())); });
            addMethod(cd, "getBytesLoaded", [](std::shared_ptr<Object> self,
                                               const std::vector<Value>&) -> Value {
                return Value::intg(static_cast<int32_t>(baBytes(self).size())); });
            addMethod(cd, "flush", noop);
            addMethod(cd, "compress", noop);
            addMethod(cd, "uncompress", noop);
            addMethod(cd, "readUTFBytes", [](std::shared_ptr<Object> self,
                                             const std::vector<Value>& a) -> Value {
                auto& b = baBytes(self);
                const std::size_t p = baPos(self);
                int n = a.empty() ? 0 : std::max(0, a[0].toInt());
                if (p >= b.size() || n == 0) return Value::str("");
                if (static_cast<std::size_t>(n) > b.size() - p) n = static_cast<int>(b.size() - p);
                std::string s(reinterpret_cast<const char*>(b.data() + p),
                              static_cast<std::size_t>(n));
                baSetPos(self, p + static_cast<std::size_t>(n));
                return Value::str(s); });
            addMethod(cd, "writeUTFBytes", [](std::shared_ptr<Object> self,
                                              const std::vector<Value>& a) -> Value {
                const std::string s = a.empty() ? std::string() : a[0].toString();
                if (s.empty()) return Value::undefined();
                auto& b = baBytes(self);
                const std::size_t p = baPos(self);
                if (p + s.size() > b.size()) b.resize(p + s.size(), 0);
                std::memcpy(b.data() + p, s.data(), s.size());
                baSetPos(self, p + s.size());
                return Value::undefined(); });
            addMethod(cd, "readUTF", [](std::shared_ptr<Object> self,
                                        const std::vector<Value>&) -> Value {
                const Value l = baReadInt(self, 2, false);
                if (l.isUndefined()) return Value::undefined();
                const int n = l.toInt();
                auto& b = baBytes(self);
                const std::size_t p = baPos(self);
                if (n <= 0) return Value::str("");
                if (p + static_cast<std::size_t>(n) > b.size()) return Value::undefined();
                std::string s(reinterpret_cast<const char*>(b.data() + p),
                              static_cast<std::size_t>(n));
                baSetPos(self, p + static_cast<std::size_t>(n));
                return Value::str(s); });
            addMethod(cd, "writeUTF", [](std::shared_ptr<Object> self,
                                         const std::vector<Value>& a) -> Value {
                const std::string s = a.empty() ? std::string() : a[0].toString();
                baWriteInt(self, 2, static_cast<double>(static_cast<int>(s.size()) & 0xFFFF));
                if (s.empty()) return Value::undefined();
                auto& b = baBytes(self);
                const std::size_t p = baPos(self);
                if (p + s.size() > b.size()) b.resize(p + s.size(), 0);
                std::memcpy(b.data() + p, s.data(), s.size());
                baSetPos(self, p + s.size());
                return Value::undefined(); });
            addMethod(cd, "writeBytes", [](std::shared_ptr<Object> self,
                                           const std::vector<Value>& a) -> Value {
                if (a.empty() || !a[0].obj) return Value::undefined();
                const auto& src = baBytes(a[0].obj);
                const std::size_t off = a.size() > 1
                    ? static_cast<std::size_t>(std::max(0, a[1].toInt())) : 0;
                if (off >= src.size()) return Value::undefined();
                std::size_t len = a.size() > 2
                    ? static_cast<std::size_t>(std::max(0, a[2].toInt())) : 0;
                if (len == 0 || off + len > src.size()) len = src.size() - off;
                auto& dst = baBytes(self);
                const std::size_t p = baPos(self);
                if (p + len > dst.size()) dst.resize(p + len, 0);
                if (len) std::memcpy(dst.data() + p, src.data() + off, len);
                baSetPos(self, p + len);
                return Value::undefined(); });
            addMethod(cd, "readBytes", [](std::shared_ptr<Object> self,
                                          const std::vector<Value>& a) -> Value {
                if (a.empty() || !a[0].obj) return Value::undefined();
                const auto& src = baBytes(self);
                const std::size_t p = baPos(self);
                if (p >= src.size()) return Value::undefined();
                const std::size_t off = a.size() > 1
                    ? static_cast<std::size_t>(std::max(0, a[1].toInt())) : 0;
                std::size_t len = a.size() > 2
                    ? static_cast<std::size_t>(std::max(0, a[2].toInt())) : 0;
                if (len == 0 || p + len > src.size()) len = src.size() - p;
                auto& dst = baBytes(a[0].obj);
                if (off + len > dst.size()) dst.resize(off + len, 0);
                if (len) std::memcpy(dst.data() + off, src.data() + p, len);
                baSetPos(self, p + len);
                return Value::undefined(); });
        }

        // ---- XML / XMLList（E4X 轻量 DOM）----
        {
            auto xmlCls = classObj("XML");
            auto listCls = classObj("XMLList");
            if (xmlCls) globals_->props["flash.xml.XML"] = Value::object(xmlCls);
            if (listCls) globals_->props["flash.xml.XMLList"] = Value::object(listCls);
            auto xcd = xmlCls ? xmlCls->classDef : nullptr;
            auto lcd = listCls ? listCls->classDef : nullptr;
            if (xcd) {
                xcd->ctor = [this](std::shared_ptr<Object> self,
                                   const std::vector<Value>& a) -> Value {
                    if (!self) return Value::undefined();
                    const std::string text = a.empty() ? std::string() : a[0].toString();
                    const Value parsed = xmlParseToValue(*this, text);
                    if (parsed.obj)
                        for (const auto& kv : parsed.obj->props) self->props[kv.first] = kv.second;
                    return Value::undefined();
                };
            }
            NativeFn nodeRaw = [](std::shared_ptr<Object> self,
                                  const std::vector<Value>&) -> Value {
                if (!self) return Value::str("");
                auto r = self->props.find("$xmlRaw");
                if (r != self->props.end()) return r->second;
                auto t = self->props.find("$xmlText");
                return t == self->props.end() ? Value::str("") : t->second;
            };
            NativeFn listRaw = [](std::shared_ptr<Object> self,
                                  const std::vector<Value>&) -> Value {
                std::string out;
                if (!self) return Value::str(out);
                auto lp = self->props.find("$length");
                const int n = lp == self->props.end() ? 0 : lp->second.toInt();
                for (int i = 0; i < n; ++i) {
                    auto it = self->props.find(std::to_string(i));
                    if (it == self->props.end() || !it->second.obj) continue;
                    const auto& p = it->second.obj->props;
                    auto r = p.find("$xmlRaw");
                    if (r != p.end()) out += r->second.toString();
                    else {
                        auto t = p.find("$xmlText");
                        if (t != p.end()) out += t->second.toString();
                    }
                }
                return Value::str(out);
            };
            auto addXmlCommon = [&](const std::shared_ptr<ClassDef>& cd, bool isList) {
                if (!cd) return;
                NativeFn raw = isList ? listRaw : nodeRaw;
                addMethod(cd, "toString", raw);
                addMethod(cd, "toXMLString", raw);
                addMethod(cd, "valueOf", [](std::shared_ptr<Object> self,
                                            const std::vector<Value>&) -> Value {
                    return Value::object(self); });
                addMethod(cd, "length", [isList](std::shared_ptr<Object> self,
                                                 const std::vector<Value>&) -> Value {
                    if (!isList) return Value::intg(1);
                    if (!self) return Value::intg(0);
                    auto it = self->props.find("$length");
                    return it == self->props.end() ? Value::intg(0) : it->second; });
                addMethod(cd, "child", [isList](std::shared_ptr<Object> self,
                                                const std::vector<Value>& a) -> Value {
                    if (!self) return Value::undefined();
                    const Value key = a.empty() ? Value::intg(0) : a[0];
                    if (isList) {
                        auto it = self->props.find(std::to_string(key.toInt()));
                        return it == self->props.end() ? Value::undefined() : it->second;
                    }
                    int n = 0;
                    const Value* ch = xmlChildValues(self, n);
                    if (!ch || !ch->obj) return Value::undefined();
                    if (key.kind == VKind::Int || key.kind == VKind::UInt ||
                        key.kind == VKind::Number) {
                        auto it = ch->obj->props.find(std::to_string(key.toInt()));
                        return it == ch->obj->props.end() ? Value::undefined() : it->second;
                    }
                    const std::string want = key.toString();
                    for (int i = 0; i < n; ++i) {
                        auto it = ch->obj->props.find(std::to_string(i));
                        if (it == ch->obj->props.end() || !it->second.obj) continue;
                        auto nm = it->second.obj->props.find("$xmlName");
                        if (nm != it->second.obj->props.end() && nm->second.toString() == want)
                            return it->second;
                    }
                    return Value::undefined(); });
                addMethod(cd, "descendants", [this, isList](std::shared_ptr<Object> self,
                                                            const std::vector<Value>& a) -> Value {
                    const std::string want = a.empty() ? std::string("*") : a[0].toString();
                    std::vector<Value> found;
                    if (self) {
                        if (isList) {
                            auto lp = self->props.find("$length");
                            const int n = lp == self->props.end() ? 0 : lp->second.toInt();
                            for (int i = 0; i < n; ++i) {
                                auto it = self->props.find(std::to_string(i));
                                if (it != self->props.end() && it->second.obj)
                                    xmlCollectDescendants(it->second.obj, want, found, 0);
                            }
                        } else xmlCollectDescendants(self, want, found, 0);
                    }
                    auto out = newObject(ObjKind::Array, "XMLList");
                    for (std::size_t k = 0; k < found.size(); ++k)
                        out->props[std::to_string(k)] = found[k];
                    out->props["$length"] = Value::intg(static_cast<int32_t>(found.size()));
                    return Value::object(out); });
                addMethod(cd, "children", [isList](std::shared_ptr<Object> self,
                                                   const std::vector<Value>&) -> Value {
                    if (isList) return Value::object(self);
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlChildren");
                    return it == self->props.end() ? Value::undefined() : it->second; });
                addMethod(cd, "elements", [isList](std::shared_ptr<Object> self,
                                                   const std::vector<Value>&) -> Value {
                    if (isList) return Value::object(self);
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlChildren");
                    return it == self->props.end() ? Value::undefined() : it->second; });
                addMethod(cd, "attributes", [](std::shared_ptr<Object> self,
                                               const std::vector<Value>&) -> Value {
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlAttrs");
                    return it == self->props.end() ? Value::undefined() : it->second; });
                addMethod(cd, "attribute", [](std::shared_ptr<Object> self,
                                              const std::vector<Value>& a) -> Value {
                    if (!self || a.empty()) return Value::undefined();
                    auto it = self->props.find("$xmlAttrs");
                    if (it == self->props.end() || !it->second.obj) return Value::undefined();
                    auto av = it->second.obj->props.find(a[0].toString());
                    return av == it->second.obj->props.end() ? Value::undefined() : av->second; });
                addMethod(cd, "name", [](std::shared_ptr<Object> self,
                                         const std::vector<Value>&) -> Value {
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlName");
                    return it == self->props.end() ? Value::undefined() : it->second; });
                addMethod(cd, "localName", [](std::shared_ptr<Object> self,
                                              const std::vector<Value>&) -> Value {
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlName");
                    if (it == self->props.end()) return Value::undefined();
                    const std::string n = it->second.toString();
                    const std::size_t colon = n.find(':');
                    return Value::str(colon == std::string::npos ? n : n.substr(colon + 1)); });
                addMethod(cd, "text", [](std::shared_ptr<Object> self,
                                         const std::vector<Value>&) -> Value {
                    if (!self) return Value::undefined();
                    auto it = self->props.find("$xmlText");
                    return it == self->props.end() ? Value::str("") : it->second; });
                addMethod(cd, "nodeKind", [](std::shared_ptr<Object>,
                                             const std::vector<Value>&) -> Value {
                    return Value::str("element"); });
                addMethod(cd, "hasSimpleContent", [](std::shared_ptr<Object> self,
                                                     const std::vector<Value>&) -> Value {
                    int n = 0; xmlChildValues(self, n); return Value::boolean(n == 0); });
                addMethod(cd, "hasComplexContent", [](std::shared_ptr<Object> self,
                                                      const std::vector<Value>&) -> Value {
                    int n = 0; xmlChildValues(self, n); return Value::boolean(n != 0); });
                for (const char* mn : {"appendChild", "prependChild", "setAttribute",
                                       "insertChildAfter", "insertChildBefore", "setChildren",
                                       "setName", "setNamespace", "replace", "copy",
                                       "normalize", "toJSON"})
                    addMethod(cd, mn, noop);
            };
            addXmlCommon(xcd, false);
            addXmlCommon(lcd, true);
            if (lcd) {
                addMethod(lcd, "getItemAt", [](std::shared_ptr<Object> self,
                                               const std::vector<Value>& a) -> Value {
                    if (!self || a.empty()) return Value::undefined();
                    auto it = self->props.find(std::to_string(a[0].toInt()));
                    return it == self->props.end() ? Value::undefined() : it->second; });
            }
        }

        // ---- TweenLite / TweenMax / Tween：按虚拟时钟插值目标属性 ----
        // vars 中除保留键外的每个数值属性都是一条补间；delay/onComplete 按 AS3 语义处理。
        static const std::vector<std::string> kReservedVars = {
            "delay", "ease", "easeParams", "onComplete", "onCompleteParams", "onCompleteScope",
            "onUpdate", "onUpdateParams", "onStart", "onStartParams", "useFrames",
            "immediateRender", "overwrite", "paused", "repeat", "repeatDelay", "yoyo",
            "onReverseComplete", "onReverseCompleteParams", "startAt", "visible"
        };
        auto buildTween = [this](const std::vector<Value>& a, int mode) -> Value {
            auto handle = newObject(ObjKind::Generic, "Tween");
            if (mode == 3) {   // delayedCall(delaySeconds, onComplete, params)
                TweenTask t;
                t.startMs = timerNowMs_;
                t.delayMs = a.empty() ? 0.0 : asNumber(a[0]) * 1000.0;
                t.durationMs = 0.0;
                if (a.size() > 1 && a[1].kind == VKind::Object && a[1].obj) {
                    t.onComplete = a[1];
                    t.hasComplete = true;
                    for (std::size_t k = 2; k < a.size(); ++k) t.completeParams.push_back(a[k]);
                    if (a.size() > 2 && a[2].obj && a[2].obj->kind == ObjKind::Array) {
                        t.completeParams.clear();
                        int n = 0;
                        auto lp = a[2].obj->props.find("$length");
                        if (lp != a[2].obj->props.end()) n = lp->second.toInt();
                        for (int k = 0; k < n; ++k) {
                            auto it = a[2].obj->props.find(std::to_string(k));
                            if (it != a[2].obj->props.end()) t.completeParams.push_back(it->second);
                        }
                    }
                }
                tweens_.push_back(std::move(t));
                return Value::object(handle);
            }
            if (a.size() < 2) return Value::object(handle);
            const double durMs = std::max(0.0, asNumber(a[1]) * 1000.0);
            Value vars, fromVars;
            if (mode == 2) {   // fromTo(target, duration, fromVars, toVars)
                if (a.size() > 2) fromVars = a[2];
                if (a.size() > 3) vars = a[3];
            } else if (a.size() > 2) vars = a[2];
            const Value* varObj = (vars.kind == VKind::Object && vars.obj) ? &vars : nullptr;
            const Value* fromObj = (fromVars.kind == VKind::Object && fromVars.obj) ? &fromVars : nullptr;
            std::vector<std::shared_ptr<Object>> targets;
            if (a[0].kind == VKind::Object && a[0].obj) {
                if (a[0].obj->kind == ObjKind::Array) {
                    int n = 0;
                    auto lp = a[0].obj->props.find("$length");
                    if (lp != a[0].obj->props.end()) n = lp->second.toInt();
                    for (int k = 0; k < n; ++k) {
                        auto it = a[0].obj->props.find(std::to_string(k));
                        if (it != a[0].obj->props.end() && it->second.obj)
                            targets.push_back(it->second.obj);
                    }
                } else targets.push_back(a[0].obj);
            }
            double delayMs = 0.0;
            Value onComplete;
            bool hasComplete = false;
            std::vector<Value> completeParams;
            if (varObj) {
                const auto& vp = varObj->obj->props;
                auto dp = vp.find("delay");
                if (dp != vp.end()) delayMs = asNumber(dp->second) * 1000.0;
                auto cp = vp.find("onComplete");
                if (cp != vp.end() && cp->second.obj) { onComplete = cp->second; hasComplete = true; }
                auto pp = vp.find("onCompleteParams");
                if (pp != vp.end()) {
                    if (pp->second.obj && pp->second.obj->kind == ObjKind::Array) {
                        int n = 0;
                        auto lp = pp->second.obj->props.find("$length");
                        if (lp != pp->second.obj->props.end()) n = lp->second.toInt();
                        for (int k = 0; k < n; ++k) {
                            auto it = pp->second.obj->props.find(std::to_string(k));
                            if (it != pp->second.obj->props.end())
                                completeParams.push_back(it->second);
                        }
                    } else completeParams.push_back(pp->second);
                }
            }
            for (auto& tg : targets) {
                TweenTask t;
                t.target = tg;
                t.startMs = timerNowMs_;
                t.durationMs = durMs;
                t.delayMs = delayMs;
                t.onComplete = onComplete;
                t.completeParams = completeParams;
                t.hasComplete = hasComplete;
                if (varObj) {
                    for (const auto& kv : varObj->obj->props) {
                        if (kv.first.empty() || kv.first[0] == '$' ||
                            std::find(kReservedVars.begin(), kReservedVars.end(), kv.first) !=
                                kReservedVars.end())
                            continue;
                        const double now = getProperty(tg, kv.first).toNumber();
                        double from = now, to = asNumber(kv.second);
                        if (mode == 1) { from = to; to = now; }
                        else if (mode == 2 && fromObj) {
                            auto fv = fromObj->obj->props.find(kv.first);
                            if (fv != fromObj->obj->props.end()) from = asNumber(fv->second);
                        }
                        t.fromTo[kv.first] = {from, to};
                    }
                }
                tweens_.push_back(std::move(t));
            }
            handle->props["$target"] = a[0];
            handle->props["$duration"] = Value::num(durMs / 1000.0);
            return Value::object(handle);
        };
        for (const char* tn : {"TweenLite", "TweenMax", "Tween"}) {
            auto co = classObj(tn);
            auto cd = co ? co->classDef : nullptr;
            if (!cd) continue;
            globals_->props[std::string("com.greensock.") + tn] = Value::object(co);
            globals_->props[std::string("gs.") + tn] = Value::object(co);
            auto wrap = [this, buildTween, cd](int mode) -> NativeFn {
                return [this, buildTween, cd, mode](std::shared_ptr<Object>,
                                                    const std::vector<Value>& a) -> Value {
                    Value h = buildTween(a, mode);
                    if (h.obj) h.obj->cls = cd;
                    return h;
                };
            };
            addMethod(cd, "to", wrap(0), true);
            addMethod(cd, "from", wrap(1), true);
            addMethod(cd, "fromTo", wrap(2), true);
            addMethod(cd, "delayedCall", wrap(3), true);
            addMethod(cd, "killTweensOf", [this](std::shared_ptr<Object>,
                                                 const std::vector<Value>& a) -> Value {
                if (!a.empty()) {
                    auto tp = a[0].obj;
                    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(),
                        [tp](const TweenTask& t) { return t.target == tp; }), tweens_.end());
                }
                return Value::undefined(); }, true);
            addMethod(cd, "killAllTweens", [this](std::shared_ptr<Object>,
                                                  const std::vector<Value>&) -> Value {
                tweens_.clear(); return Value::undefined(); }, true);
            addMethod(cd, "killAll", [this](std::shared_ptr<Object>,
                                            const std::vector<Value>&) -> Value {
                tweens_.clear(); return Value::undefined(); }, true);
            addMethod(cd, "kill", [this](std::shared_ptr<Object> self,
                                         const std::vector<Value>&) -> Value {
                if (self) {
                    auto it = self->props.find("$target");
                    auto tp = it == self->props.end() ? nullptr : it->second.obj;
                    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(),
                        [tp](const TweenTask& t) { return t.target == tp; }), tweens_.end());
                }
                return Value::undefined(); });
        }

        // ---- 加载 / 网络 / 媒体 / 本地存储：无外部 I/O，但对象结构与常用方法完整 ----
        if (auto cd = classDefOf("URLRequest")) {
            globals_->props["flash.net.URLRequest"] = Value::object(classObj("URLRequest"));
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
                if (!self) return Value::undefined();
                self->props["url"] = a.empty() ? Value::str("") : a[0];
                self->props["method"] = Value::str("GET");
                self->props["data"] = Value::undefined();
                return Value::undefined();
            };
        }
        if (auto cd = classDefOf("URLLoader")) {
            globals_->props["flash.net.URLLoader"] = Value::object(classObj("URLLoader"));
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self) return Value::undefined();
                self->props["data"] = Value::str("");
                self->props["dataFormat"] = Value::str("text");
                self->props["bytesLoaded"] = Value::intg(0);
                self->props["bytesTotal"] = Value::intg(0);
                return Value::undefined();
            };
            addMethod(cd, "load", [this](std::shared_ptr<Object> self,
                                         const std::vector<Value>& a) -> Value {
                if (!self) return Value::undefined();
                if (!a.empty() && a[0].obj) {
                    auto u = a[0].obj->props.find("url");
                    if (u != a[0].obj->props.end()) self->props["url"] = u->second;
                }
                self->props["data"] = Value::str("");
                auto it = globals_->props.find("Event");
                if (it != globals_->props.end() && it->second.obj) {
                    const Value ev = constructObject(it->second, {Value::str("complete")});
                    callMethod(self, "dispatchEvent", {ev});
                }
                return Value::undefined(); });
        }
        if (auto cd = classDefOf("LoaderInfo")) {
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self) return Value::undefined();
                // 本地整片加载：按已完整加载处理（同 DisplayObject.loaderInfo getter）
                self->props["bytesLoaded"] = Value::intg(100);
                self->props["bytesTotal"] = Value::intg(100);
                self->props["url"] = Value::str("file://local.swf");
                return Value::undefined();
            };
        }
        if (auto cd = classDefOf("Loader")) {
            globals_->props["flash.display.Loader"] = Value::object(classObj("Loader"));
            cd->ctor = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (self)
                    self->props["contentLoaderInfo"] =
                        Value::object(newObject(ObjKind::Generic, "LoaderInfo"));
                return Value::undefined();
            };
            addMethod(cd, "load", [this](std::shared_ptr<Object> self,
                                         const std::vector<Value>&) -> Value {
                if (self)
                    self->props["content"] = Value::object(newObject(ObjKind::Sprite, "Sprite"));
                return Value::undefined(); });
            addMethod(cd, "unload", noop);
            addMethod(cd, "unloadAndStop", noop);
        }
        if (auto cd = classDefOf("Sound")) {
            globals_->props["flash.media.Sound"] = Value::object(classObj("Sound"));
            addMethod(cd, "play", [this](std::shared_ptr<Object>,
                                         const std::vector<Value>&) -> Value {
                auto it = globals_->props.find("SoundChannel");
                if (it == globals_->props.end() || !it->second.obj) return Value::undefined();
                return constructObject(it->second, {}); });
        }
        if (auto cd = classDefOf("SoundChannel")) {
            Member g; g.kind = Member::Getter; g.name = "position";
            g.native = [](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
                return Value::num(0.0); };
            cd->instanceMembers.push_back(g);
        }
        if (auto cd = classDefOf("NetConnection")) {
            globals_->props["flash.net.NetConnection"] = Value::object(classObj("NetConnection"));
            cd->ctor = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (self) self->props["connected"] = Value::boolean(false);
                return Value::undefined();
            };
            addMethod(cd, "connect", [](std::shared_ptr<Object> self,
                                        const std::vector<Value>& a) -> Value {
                if (self) {
                    self->props["connected"] = Value::boolean(true);
                    self->props["uri"] = a.empty() ? Value::str("") : a[0];
                }
                return Value::boolean(true); });
            addMethod(cd, "call", noop);
        }
        if (auto cd = classDefOf("NetStream")) {
            globals_->props["flash.net.NetStream"] = Value::object(classObj("NetStream"));
            cd->ctor = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (self) self->props["client"] = Value::object(newObject(ObjKind::Generic, "Object"));
                return Value::undefined();
            };
            addMethod(cd, "attach", noop);
            addMethod(cd, "seek", noop);
        }
        if (auto cd = classDefOf("SharedObject")) {
            globals_->props["flash.net.SharedObject"] = Value::object(classObj("SharedObject"));
            addMethod(cd, "getLocal", [this](std::shared_ptr<Object>,
                                             const std::vector<Value>&) -> Value {
                auto so = newObject(ObjKind::Generic, "SharedObject");
                so->props["data"] = Value::object(newObject(ObjKind::Generic, "Object"));
                return Value::object(so); }, true);
        }
        // applicationDomain：Alchemy 的 domainMemory 通过 Vm::setProperty 绑定字节缓冲。
        // currentDomain 是实例；同名全局符号是类对象，hasDefinition/getDefinition
        // 按 AS3 语义查询全局类表（全名与短名都可命中）。
        {
            auto ad = newObject(ObjKind::Generic, "ApplicationDomain");
            auto cd = std::make_shared<ClassDef>();
            cd->name = "ApplicationDomain";
            ad->cls = cd;
            auto hasDef = [this](const std::vector<Value>& a) -> bool {
                if (a.empty()) return false;
                const std::string q = a[0].toString();
                if (globals_->props.count(q)) return true;
                const std::size_t dot = q.rfind('.');
                return dot != std::string::npos && globals_->props.count(q.substr(dot + 1));
            };
            addMethod(cd, "hasDefinition", [hasDef](std::shared_ptr<Object>,
                                                     const std::vector<Value>& a) -> Value {
                return Value::boolean(hasDef(a)); });
            addMethod(cd, "getDefinition", [this, hasDef](std::shared_ptr<Object>,
                                                          const std::vector<Value>& a) -> Value {
                if (a.empty() || !hasDef(a)) return Value::undefined();
                const std::string q = a[0].toString();
                auto it = globals_->props.find(q);
                if (it == globals_->props.end()) {
                    const std::size_t dot = q.rfind('.');
                    if (dot != std::string::npos) it = globals_->props.find(q.substr(dot + 1));
                }
                return it == globals_->props.end() ? Value::undefined() : it->second; });
            addMethod(cd, "hasOwnProperty", [this](std::shared_ptr<Object>,
                                                   const std::vector<Value>& a) -> Value {
                if (a.empty()) return Value::boolean(false);
                return Value::boolean(globals_->props.count(a[0].toString()) != 0); });
            addMethod(cd, "toString", [](std::shared_ptr<Object>,
                                         const std::vector<Value>&) -> Value {
                return Value::str("[object ApplicationDomain]"); });
            auto cls = newObject(ObjKind::Class, "ApplicationDomain");
            cls->classDef = cd;
            cls->props["currentDomain"] = Value::object(ad);
            ad->props["currentDomain"] = Value::object(ad);
            globals_->props["applicationDomain"] = Value::object(ad);
            globals_->props["ApplicationDomain"] = Value::object(cls);
            globals_->props["flash.system.ApplicationDomain"] = Value::object(cls);
        }
    }

    // ---- 全局函数 ----
    // 此前用 undefined 占位，getlex 取出的函数值无法被调用（var f = parseInt; f(s)）。
    // 统一注册为绑定 $this=全局对象的原生函数对象；callproperty 仍按名字走 callMethod。
    // 注意 String/Number/Math 等类对象不能被覆盖。
    for (const char* g : {"parseInt", "parseFloat", "isNaN", "isFinite", "int", "uint",
                          "Boolean", "getTimer", "trace",
                          "getDefinitionByName", "getQualifiedClassName",
                          "getQualifiedSuperclassName", "describeType",
                          "encodeURI", "encodeURIComponent", "decodeURI",
                          "decodeURIComponent", "escape", "unescape",
                          "setTimeout", "setInterval", "clearTimeout", "clearInterval",
                          "navigateToURL"}) {
        auto f = newObject(ObjKind::Function, g);
        f->props["$native"] = Value::str(g);
        f->props["$this"] = Value::object(globals_);
        globals_->props[g] = Value::object(f);
    }
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
        // AS3: addEventListener(type, listener, useCapture=false, priority=0, useWeakReference=false)
        // 捕获监听器单独存 "$cap:<type>"；优先级存 "$prio:<key>"（与监听器一一对应）。
        Member add; add.kind = Member::Method; add.name = "addEventListener";
        add.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.size() < 2) return Value::undefined();
            const bool capture = a.size() > 2 && a[2].toBool();
            const int priority = a.size() > 3 ? asIntCast(asNumber(a[3])) : 0;
            const std::string type = listenerKey(a[0]);
            const std::string key = (capture ? "$cap:" : "") + type;
            auto& ls = self->listeners[key];
            for (const auto& lv : ls)
                if (sameListener(lv, a[1])) return Value::undefined();  // 同一监听器不重复注册
            ls.push_back(a[1]);
            self->listeners["$prio:" + key].push_back(Value::intg(priority));
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] addEventListener self=%s key=%s capture=%d priority=%d cid=%d\n",
                             self->className.c_str(), key.c_str(), (int)capture, priority,
                             self->display ? (int)self->display->characterId : -1);
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(add);
        Member rem; rem.kind = Member::Method; rem.name = "removeEventListener";
        rem.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::undefined();
            const bool capture = a.size() > 2 && a[2].toBool();
            const std::string key = (capture ? "$cap:" : "") + listenerKey(a[0]);
            auto lit = self->listeners.find(key);
            if (lit == self->listeners.end()) return Value::undefined();
            auto pit = self->listeners.find("$prio:" + key);
            // 未传监听器时清空该类型的全部监听器（并同步优先级表）。
            if (a.size() < 2 || a[1].kind != VKind::Object || !a[1].obj) {
                self->listeners.erase(lit);
                if (pit != self->listeners.end()) self->listeners.erase(pit);
                return Value::undefined();
            }
            auto& ls = lit->second;
            for (std::size_t i = 0; i < ls.size(); ++i) {
                if (!sameListener(ls[i], a[1])) continue;
                ls.erase(ls.begin() + static_cast<std::ptrdiff_t>(i));
                if (pit != self->listeners.end() && i < pit->second.size())
                    pit->second.erase(pit->second.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            if (ls.empty()) {
                self->listeners.erase(lit);
                if (pit != self->listeners.end()) self->listeners.erase(pit);
            }
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(rem);
        // dispatchEvent：完整的三段事件流（捕获 -> 目标 -> 冒泡），
        // 设置 target/currentTarget/eventPhase，尊重 stopPropagation /
        // stopImmediatePropagation / preventDefault，返回是否未被取消。
        Member disp; disp.kind = Member::Method; disp.name = "dispatchEvent";
        disp.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty() || a[0].kind != VKind::Object || !a[0].obj) return Value::boolean(false);
            const Value ev = a[0];
            const std::string type = listenerKey(ev);
            ev.obj->props["target"] = Value::object(self);
            ev.obj->props["$stop"] = Value::boolean(false);
            ev.obj->props["$stopImmediate"] = Value::boolean(false);

            // self -> parent -> ... -> Stage
            std::vector<std::shared_ptr<Object>> chain{self};
            if (self->display) {
                auto p = self->display->parent;
                for (int guard = 0; p && guard < 64; ++guard) {
                    chain.push_back(objectForDisplay(p));
                    p = p->parent;
                }
                for (const auto& root : stage) {
                    if (!root) continue;
                    bool present = false;
                    for (const auto& o : chain)
                        if (o && o->display == root) { present = true; break; }
                    if (!present) chain.push_back(objectForDisplay(root));
                }
            }
            auto flag = [&](const char* n) {
                const auto it = ev.obj->props.find(n);
                return it != ev.obj->props.end() && it->second.toBool();
            };
            auto fire = [&](const std::shared_ptr<Object>& node, const std::string& key, int phase) {
                if (!node || flag("$stopImmediate")) return;
                ev.obj->props["currentTarget"] = Value::object(node);
                ev.obj->props["eventPhase"] = Value::intg(phase);
                const auto lit = node->listeners.find(key);
                if (lit == node->listeners.end()) return;
                const std::vector<Value> snapshot = lit->second;  // 回调可增删监听器
                for (const std::size_t idx : listenerOrder(node, key, snapshot.size())) {
                    if (flag("$stopImmediate")) break;
                    invokeValue(snapshot[idx], {ev}, node);
                }
            };
            const std::string capKey = "$cap:" + type;
            for (std::size_t i = chain.size(); i-- > 1;) {   // 捕获：外 -> 内
                fire(chain[i], capKey, 1);
                if (flag("$stop")) break;
            }
            if (!flag("$stop") && !flag("$stopImmediate")) {  // 目标：两类监听器都触发
                fire(self, capKey, 2);
                fire(self, type, 2);
            }
            const auto bp = ev.obj->props.find("bubbles");
            const bool bubbles = bp != ev.obj->props.end() && bp->second.toBool();
            if (bubbles && !flag("$stop") && !flag("$stopImmediate")) {
                for (std::size_t i = 1; i < chain.size(); ++i) {  // 冒泡：内 -> 外
                    fire(chain[i], type, 3);
                    if (flag("$stop")) break;
                }
            }
            ev.obj->props["currentTarget"] = Value::nullv();
            const auto cp = ev.obj->props.find("cancelable");
            const bool cancelable = cp != ev.obj->props.end() && cp->second.toBool();
            return Value::boolean(!(cancelable && flag("$defaultPrevented")));
        };
        c->classDef->instanceMembers.push_back(disp);
        Member has; has.kind = Member::Method; has.name = "hasEventListener";
        has.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::boolean(false);
            const std::string type = listenerKey(a[0]);
            for (const auto& kv : self->listeners) {
                if (kv.first.compare(0, 6, "$prio:") == 0 || kv.second.empty()) continue;
                if (kv.first == type || kv.first == "$cap:" + type) return Value::boolean(true);
            }
            return Value::boolean(false);
        };
        c->classDef->instanceMembers.push_back(has);
        Member will; will.kind = Member::Method; will.name = "willTrigger";
        will.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || a.empty()) return Value::boolean(false);
            const std::string type = listenerKey(a[0]);
            auto hasType = [&](const std::shared_ptr<Object>& o) {
                if (!o) return false;
                for (const auto& kv : o->listeners) {
                    if (kv.first.compare(0, 6, "$prio:") == 0 || kv.second.empty()) continue;
                    if (kv.first == type || kv.first == "$cap:" + type) return true;
                }
                return false;
            };
            if (hasType(self)) return Value::boolean(true);
            auto p = self->display ? self->display->parent : nullptr;
            for (int guard = 0; p && guard < 64; ++guard) {
                if (hasType(objectForDisplay(p))) return Value::boolean(true);
                p = p->parent;
            }
            return Value::boolean(false);
        };
        c->classDef->instanceMembers.push_back(will);
    }

    // ---- DisplayObject / Sprite / MovieClip（显示列表操作）----
    auto addDisplayMembers = [this](std::shared_ptr<ClassDef> cd, ObjKind k) {
        Member addChild; addChild.kind = Member::Method; addChild.name = "addChild";
        addChild.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] addChild ENTER self=%s disp=%d argc=%zu child=%s childDisp=%d\n",
                             self ? self->className.c_str() : "-", (int)(self && self->display), a.size(),
                             (!a.empty() && a[0].obj) ? a[0].obj->className.c_str() : "-",
                             (!a.empty() && a[0].obj) ? (int)(bool)a[0].obj->display : -1);
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return a.empty() ? Value::undefined() : a[0];
            auto child = a[0].obj->display;
            detachFromParent(child);
            child->parent = self->display;
            child->depth = -1;   // 脚本添加的对象没有 SWF PlaceObject depth
            self->display->children.push_back(child);
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] addChild parent=%s child=%s name='%s'\n",
                             self->className.c_str(), a[0].obj->className.c_str(), child->name.c_str());
            return a[0];
        };
        cd->instanceMembers.push_back(addChild);
        Member removeChild; removeChild.kind = Member::Method; removeChild.name = "removeChild";
        removeChild.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty() || !a[0].obj || !a[0].obj->display) return Value::undefined();
            auto& ch = self->display->children;
            for (auto it = ch.begin(); it != ch.end(); ++it)
                if (it->get() == a[0].obj->display.get()) {
                    ch.erase(it);
                    a[0].obj->display->parent.reset();   // Flash：移除后 parent 变为 null
                    break;
                }
            return a[0];
        };
        cd->instanceMembers.push_back(removeChild);
        // addFrameScript(frameIndex, func, frameIndex2, func2, ...) —— 进入对应帧时执行。
        // AS3 允许一次传入多对参数（main 剪辑构造函数就一次性注册了 frame7 与 frame8 两对）。
        Member afs; afs.kind = Member::Method; afs.name = "addFrameScript";
        afs.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (self && self->display) {
                for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                    // Flash 语义：addFrameScript 的帧索引是 0 基（0 = 第 1 帧）；
                    // VM 内部 currentFrame 同为 0 基（AS3 getter 才 +1），直接存储。
                    int frame = a[i].toInt();
                    Value fn = a[i + 1];
                    if (fn.isNullOrUndef()) {
                        self->display->frameScripts.erase(frame);
                    } else {
                        self->display->frameScripts[frame] = fn;
                    }
                    if (std::getenv("SWF_AS_DEBUG"))
                        std::fprintf(stderr, "[as] addFrameScript self=%s frame=%d valKind=%d cls=%s\n",
                                     self->className.c_str(), frame, static_cast<int>(fn.kind),
                                     (fn.obj) ? fn.obj->className.c_str() : "-");
                }
            }
            return Value::undefined();
        };
        cd->instanceMembers.push_back(afs);
        Member numChildren; numChildren.kind = Member::Getter; numChildren.name = "numChildren";
        numChildren.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::intg(self && self->display ? static_cast<int32_t>(self->display->children.size()) : 0);
        };
        cd->instanceMembers.push_back(numChildren);
        // getChildAt(i) —— 若该显示对象已有 AS3 实例则返回原实例，保留属性/状态
        Member gca; gca.kind = Member::Method; gca.name = "getChildAt";
        gca.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::nullv();
            int i = a[0].toInt();
            const auto& ch = self->display->children;
            if (i < 0 || i >= static_cast<int>(ch.size())) return Value::nullv();
            if (auto ow = ch[i]->owner.lock()) return Value::object(ow);
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
                if (c && c->name == n) return Value::object(objectForDisplay(c));
            }
            if (std::getenv("SWF_AS_DEBUG")) {
                std::string kids;
                for (auto& c : self->display->children)
                    kids += c->name + "(" + (c->owner.lock() ? c->owner.lock()->className : std::string("-")) +
                            ",cid" + std::to_string(c->characterId) + ")|";
                std::fprintf(stderr, "[as] getChildByName MISS self=%s want='%s' kids=%s\n",
                             self->className.c_str(), n.c_str(), kids.c_str());
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
            if (i >= 0 && i < static_cast<int>(ch.size())) {
                auto removed = ch[i];
                ch.erase(ch.begin() + i);
                if (removed->parent == self->display) removed->parent.reset();
                if (auto ow = removed->owner.lock()) return Value::object(ow);
                auto o = std::make_shared<Object>();
                o->kind = ObjKind::MovieClip;
                o->className = "DisplayObject";
                o->display = removed;
                return Value::object(o);
            }
            return Value::nullv();
        };
        cd->instanceMembers.push_back(rca);
        // 层级操作：Flash 动态创建的元件常按指定深度插入/换层，缺失会静默失败。
        Member aca; aca.kind = Member::Method; aca.name = "addChildAt";
        aca.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.size() < 2 || !a[0].obj || !a[0].obj->display)
                return a.empty() ? Value::undefined() : a[0];
            auto child = a[0].obj->display;
            detachFromParent(child);
            auto& ch = self->display->children;
            child->parent = self->display;
            child->depth = -1;
            int i = a[1].toInt();
            if (i < 0) i = 0;
            if (i > static_cast<int>(ch.size())) i = static_cast<int>(ch.size());
            ch.insert(ch.begin() + i, child);
            return a[0];
        };
        cd->instanceMembers.push_back(aca);
        Member sci; sci.kind = Member::Method; sci.name = "setChildIndex";
        sci.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.size() < 2 || !a[0].obj || !a[0].obj->display)
                return Value::undefined();
            auto child = a[0].obj->display;
            auto& ch = self->display->children;
            auto it = std::find_if(ch.begin(), ch.end(),
                                   [&](const std::shared_ptr<DisplayObject>& c) { return c.get() == child.get(); });
            if (it == ch.end()) return Value::undefined();
            ch.erase(it);
            int i = a[1].toInt();
            if (i < 0) i = 0;
            if (i > static_cast<int>(ch.size())) i = static_cast<int>(ch.size());
            ch.insert(ch.begin() + i, child);
            return Value::undefined();
        };
        cd->instanceMembers.push_back(sci);
        Member swc; swc.kind = Member::Method; swc.name = "swapChildren";
        swc.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.size() < 2 || !a[0].obj || !a[0].obj->display ||
                !a[1].obj || !a[1].obj->display) return Value::undefined();
            auto& ch = self->display->children;
            auto p = ch.end(), q = ch.end();
            for (auto it = ch.begin(); it != ch.end(); ++it) {
                if (it->get() == a[0].obj->display.get()) p = it;
                if (it->get() == a[1].obj->display.get()) q = it;
            }
            if (p != ch.end() && q != ch.end()) std::iter_swap(p, q);
            return Value::undefined();
        };
        cd->instanceMembers.push_back(swc);
        // swapChildrenAt(i, j) / removeChildren(begin=0, end=numChildren)
        Member swa; swa.kind = Member::Method; swa.name = "swapChildrenAt";
        swa.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.size() < 2) return Value::undefined();
            auto& ch = self->display->children;
            const int i = a[0].toInt(), j = a[1].toInt();
            if (i >= 0 && j >= 0 && i < static_cast<int>(ch.size()) && j < static_cast<int>(ch.size()))
                std::iter_swap(ch.begin() + i, ch.begin() + j);
            return Value::undefined();
        };
        cd->instanceMembers.push_back(swa);
        Member rmc; rmc.kind = Member::Method; rmc.name = "removeChildren";
        rmc.native = [](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display) return Value::undefined();
            auto& ch = self->display->children;
            int lo = a.empty() ? 0 : a[0].toInt();
            int hi = a.size() > 1 ? a[1].toInt() : static_cast<int>(ch.size());
            if (lo < 0) lo = 0;
            if (hi > static_cast<int>(ch.size())) hi = static_cast<int>(ch.size());
            for (int k = lo; k < hi && k < static_cast<int>(ch.size()); ++k)
                if (ch[static_cast<std::size_t>(k)]->parent == self->display)
                    ch[static_cast<std::size_t>(k)]->parent.reset();
            if (lo < hi && lo < static_cast<int>(ch.size()))
                ch.erase(ch.begin() + lo, ch.begin() + std::min(hi, static_cast<int>(ch.size())));
            return Value::undefined();
        };
        cd->instanceMembers.push_back(rmc);
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
            if (auto ow = self->display->parent->owner.lock()) return Value::object(ow);
            auto o = newObject(ObjKind::MovieClip, "DisplayObject");
            o->display = self->display->parent;
            return Value::object(o);
        };
        cd->instanceMembers.push_back(par);
        // 显示对象的几何/外观属性 getter（与 setProperty 的同步字段对应）
        for (const char* pn : {"x", "y", "scaleX", "scaleY", "rotation", "alpha"}) {
            Member gm; gm.kind = Member::Getter; gm.name = pn;
            gm.native = [pn](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self || !self->display) return Value::num(0);
                const auto& d = *self->display;
                std::string n = pn;
                if (n == "x") return Value::num(d.x);
                if (n == "y") return Value::num(d.y);
                if (n == "scaleX") return Value::num(d.scaleX);
                if (n == "scaleY") return Value::num(d.scaleY);
                if (n == "rotation") return Value::num(d.rotation);
                return Value::num(d.alpha);
            };
            cd->instanceMembers.push_back(gm);
        }
        Member visG; visG.kind = Member::Getter; visG.name = "visible";
        visG.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::boolean(self && self->display && self->display->visible);
        };
        cd->instanceMembers.push_back(visG);
        for (const char* pn : {"mouseEnabled", "mouseChildren"}) {
            Member gm; gm.kind = Member::Getter; gm.name = pn;
            gm.native = [pn](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self || !self->display) return Value::boolean(true);
                return Value::boolean(std::string(pn) == "mouseEnabled"
                                          ? self->display->mouseEnabled
                                          : self->display->mouseChildren);
            };
            cd->instanceMembers.push_back(gm);
        }
        Member nameG; nameG.kind = Member::Getter; nameG.name = "name";
        nameG.native = [](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            return Value::str(self && self->display ? self->display->name : std::string());
        };
        cd->instanceMembers.push_back(nameG);
        Member stageM; stageM.kind = Member::Getter; stageM.name = "stage";
        stageM.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self || !self->display) return Value::nullv();
            auto d = self->display;
            while (d->parent) d = d->parent;
            bool isRoot = false;
            for (auto& s : stage) if (s.get() == d.get()) { isRoot = true; break; }
            if (!isRoot) return Value::nullv();
            // 同一根舞台缓存同一 Stage 实例，保证 === 比较与事件监听一致
            auto& cache = stageCache_[d.get()];
            if (!cache) {
                cache = newObject(ObjKind::Stage, "Stage");
                cache->display = d;
                auto st = globals_->props.find("Stage");   // Stage 继承 DisplayObject 的成员
                if (st != globals_->props.end() && st->second.kind == VKind::Object && st->second.obj)
                    cache->cls = st->second.obj->classDef;
            }
            return Value::object(cache);
        };
        cd->instanceMembers.push_back(stageM);
        // loaderInfo：文档类/显示对象的加载信息。本地整片加载语义下视为已完整加载
        // （bytesLoaded==bytesTotal>0），否则 GameMain 等文档类的加载轮询永远不通过，
        // 无法驱动主时间轴 gotoAndStop 到门户/标题屏（表现为主时间轴自由播放、跳过开场）。
        Member liG; liG.kind = Member::Getter; liG.name = "loaderInfo";
        liG.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self || !self->display) return Value::nullv();
            auto& cache = self->props["loaderInfo"];
            if (cache.kind == VKind::Object && cache.obj) return cache;
            auto info = newObject(ObjKind::Generic, "LoaderInfo");
            info->props["bytesLoaded"] = Value::intg(100);
            info->props["bytesTotal"] = Value::intg(100);
            info->props["url"] = Value::str("file://local.swf");
            info->props["content"] = Value::object(self);
            info->props["contentLoaderInfo"] = Value::object(info);
            loaderInfos_.push_back(info);
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] loaderInfo accessed self=%s\n", self->className.c_str());
            cache = Value::object(info);
            return cache;
        };
        cd->instanceMembers.push_back(liG);
        // root：Flash 语义 = 主 SWF 的文档类实例（MovieClip 子类的时间轴对象），
        // 而不是 Stage 单例；stage 才是 Stage。preloader 的 progreso 用
        // MovieClip(root).loaderInfo.bytesTotal 取真实字节。
        // 闭包/监听处理器里的 `root` 经作用域链解析到 Stage 包装对象时，
        // 该对象的类链没有 play/gotoAndStop 等时间轴方法，MovieClip(root).play()
        // 会静默失效，导致门户的 Play 按钮无法把根时间轴推进到游戏帧。
        // 因此统一返回舞台根时间轴（cid=0）的 AS3 实例。
        Member rtG; rtG.kind = Member::Getter; rtG.name = "root";
        rtG.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self || !self->display) return Value::nullv();
            for (auto& s : stage) {
                if (s && s->characterId == 0) {
                    if (auto ow = s->owner.lock()) return Value::object(ow);
                    break;
                }
            }
            auto d = self->display;
            while (d->parent) d = d->parent;
            return Value::object(objectForDisplay(d));
        };
        cd->instanceMembers.push_back(rtG);
        // mouseX / mouseY：指针在对象局部坐标系中的像素位置
        for (const char* pn : {"mouseX", "mouseY"}) {
            Member mm; mm.kind = Member::Getter; mm.name = pn;
            mm.native = [this, pn](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
                if (!self || !self->display) return Value::num(0);
                double lx = 0, ly = 0;
                if (!displayLocalPoint(self->display, movie_, mouseX_, mouseY_, lx, ly)) return Value::num(0);
                return Value::num(std::string(pn) == "mouseX" ? lx : ly);
            };
            cd->instanceMembers.push_back(mm);
        }
        // hitTestPoint(x, y, shapeFlag=false)：舞台像素坐标；shapeFlag 为 true 时按实际几何判定
        Member htp; htp.kind = Member::Method; htp.name = "hitTestPoint";
        htp.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.size() < 2) return Value::boolean(false);
            bool shape = a.size() >= 3 && a[2].toBool();
            bool r = hitTestRuntimeObject(movie_, self->display,
                                          a[0].toNumber(), a[1].toNumber(), shape);
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] hitTestPoint self=%s cid=%u name=%s (%.1f,%.1f) shape=%d -> %d\n",
                             self->className.c_str(), static_cast<unsigned>(self->display->characterId),
                             self->display->name.c_str(), a[0].toNumber(), a[1].toNumber(),
                             static_cast<int>(shape), static_cast<int>(r));
            return Value::boolean(r);
        };
        cd->instanceMembers.push_back(htp);
        (void)k;
    };

    // 原生类继承链：DisplayObject → EventDispatcher，Sprite/MovieClip/TextField 逐级继承，
    // 这样 AS3 子类沿 superClass 链能访问 addEventListener 等基类成员。
    auto inheritNative = [this](const std::shared_ptr<ClassDef>& cd, const char* baseName) {
        auto p = globals_->props.find(baseName);
        if (p != globals_->props.end() && p->second.kind == VKind::Object && p->second.obj)
            cd->superClass = p->second.obj->classDef;
    };
    {
        auto c = mkClass("DisplayObject", ObjKind::DisplayObject);
        inheritNative(c->classDef, "EventDispatcher");
        addDisplayMembers(c->classDef, ObjKind::DisplayObject);
    }
    // Flash 原生类链的两个中间类型此前没有类对象：ABC 里
    // `getChildByName("btnPlay") as InteractiveObject` 的 getlex 取到 undefined，
    // astypelate 于是返回 null，标题屏 PLAY / MORE GAMES 的 CLICK 监听注册被跳过。
    {
        auto c = mkClass("InteractiveObject", ObjKind::InteractiveObject);
        inheritNative(c->classDef, "DisplayObject");
        addDisplayMembers(c->classDef, ObjKind::InteractiveObject);
    }
    {
        auto c = mkClass("DisplayObjectContainer", ObjKind::DisplayObjectContainer);
        inheritNative(c->classDef, "InteractiveObject");
        addDisplayMembers(c->classDef, ObjKind::DisplayObjectContainer);
    }
    // DefineButton2 角色在 ABC 中以 flash.display.SimpleButton 访问。
    // 关卡 play 里 `getChildByName("nextBtn") as SimpleButton` / skipBtn / btnMore
    // 取不到类对象就会得到 null，next / skip 的 CLICK 监听全部注册不上。
    {
        auto c = mkClass("SimpleButton", ObjKind::InteractiveObject);
        inheritNative(c->classDef, "InteractiveObject");
        addDisplayMembers(c->classDef, ObjKind::InteractiveObject);
    }
    {
        auto c = mkClass("Sprite", ObjKind::Sprite);
        inheritNative(c->classDef, "DisplayObjectContainer");
        addDisplayMembers(c->classDef, ObjKind::Sprite);
        Member g; g.kind = Member::Getter; g.name = "graphics";
        g.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            if (!self) return Value::undefined();
            // Flash 语义：重复访问 sprite.graphics 返回同一 Graphics 实例；实例经
            // gfxTarget 绑定宿主显示对象，绘制命令落进宿主 DisplayObject::graphics。
            auto cached = self->props.find("$graphics");
            if (cached != self->props.end() && cached->second.kind == VKind::Object &&
                cached->second.obj)
                return cached->second;
            auto go = newObject(ObjKind::Graphics, "Graphics");
            go->props["$owner"] = Value::object(self);
            go->gfxTarget = self->display;
            self->props["$graphics"] = Value::object(go);
            return Value::object(go);
        };
        c->classDef->instanceMembers.push_back(g);
        // startDrag(lockCenter=false, bounds=null)：拖动期间对象跟随指针（由 tick 前的鼠标移动更新）
        Member sdm; sdm.kind = Member::Method; sdm.name = "startDrag";
        sdm.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display) return Value::undefined();
            auto d = self->display;
            bool lockCenter = !a.empty() && a[0].toBool();
            dragTarget_ = d;
            dragOffsetX_ = dragOffsetY_ = 0.0;
            if (d->parent) {
                double lx = 0, ly = 0;
                if (displayLocalPoint(d->parent, movie_, mouseX_, mouseY_, lx, ly)) {
                    dragOffsetX_ = lockCenter ? 0.0 : lx - d->x;
                    dragOffsetY_ = lockCenter ? 0.0 : ly - d->y;
                }
            }
            if (std::getenv("SWF_AS_DEBUG"))
                std::fprintf(stderr, "[as] startDrag self=%s cid=%u name=%s depth=%d parent=%s lock=%d\n",
                             self->className.c_str(), static_cast<unsigned>(d->characterId),
                             d->name.c_str(), d->depth, d->parent ? d->parent->name.c_str() : "-",
                             static_cast<int>(lockCenter));
            updateDragPosition();
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(sdm);
        // stopDrag() 结束的是舞台级拖动状态：Flash 允许在任意 Sprite 上调用，
        // 不要求调用者就是 startDrag() 的那个对象。
        Member sdm2; sdm2.kind = Member::Method; sdm2.name = "stopDrag";
        sdm2.native = [this](std::shared_ptr<Object>, const std::vector<Value>&) -> Value {
            dragTarget_.reset();
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(sdm2);
    }
    {
        auto c = mkClass("MovieClip", ObjKind::MovieClip);
        inheritNative(c->classDef, "Sprite");
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
            // AS3 语义：currentFrame 为 1 基（内部 currentFrame 为 0 基索引）。
            return Value::intg(self && self->display ? self->display->currentFrame + 1 : 1);
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
            // 主时间轴（文档类 / root，characterId == 0）的总帧数即影片主时间轴帧数，
            // 不能为 1，否则 getFrameIndexByLabel 的 Math.min(result, totalFrames-1)
            // 会把合法标签钳成 0，并误触发“帧标签不存在”报错。
            return Value::intg(static_cast<int32_t>(movie_.frames.size()));
        };
        c->classDef->instanceMembers.push_back(tf);
        // currentLabels：帧标签数组（元素含 name / frame，frame 为 1 基），
        // FrameUtils.getFrameIndexByLabel 依赖它把字符串标签解析为帧号。
        Member cl; cl.kind = Member::Getter; cl.name = "currentLabels";
        cl.native = [this](std::shared_ptr<Object> self, const std::vector<Value>&) -> Value {
            auto o = newObject(ObjKind::Array, "Array");
            auto ac = globals_->props.find("Array");
            if (ac != globals_->props.end() && ac->second.kind == VKind::Object && ac->second.obj)
                o->cls = ac->second.obj->classDef;
            // 文档类/root 的时间轴就是主时间轴。self->display 为空的纯 AS3 包装
            // 对象也按 root 处理：否则 currentLabels 返回空数组，AS3 的按标签跳转
            // （FrameUtils.getFrameIndexByLabel）会把合法标签误判为不存在。
            const std::map<std::string, int>* labels = &movie_.frameLabels;
            if (self && self->display && self->display->characterId != 0) {
                auto it = movie_.characters.find(self->display->characterId);
                if (it != movie_.characters.end() && it->second &&
                    it->second->kind == Character::Kind::Sprite &&
                    !it->second->frameLabels.empty())
                    labels = &it->second->frameLabels;
            }
            int n = 0;
            if (std::getenv("SWF_AS_DEBUG") && self &&
                self->className.find("GameMain") != std::string::npos) {
                std::string labList;
                if (labels) for (const auto& kv : *labels)
                    labList += "'" + kv.first + "'=" + std::to_string(kv.second) + " ";
                std::fprintf(stderr, "[lbl-GM] cid=%d cur=%d [%s]\n",
                    self->display ? self->display->characterId : -1,
                    self->display ? self->display->currentFrame : -1,
                    labList.c_str());
            }
            if (labels) {
                for (const auto& kv : *labels) {
                    auto l = newObject(ObjKind::Generic, "Object");
                    l->props["name"] = Value::str(kv.first);
                    l->props["frame"] = Value::intg(kv.second);
                    o->props[std::to_string(n++)] = Value::object(l);
                }
            }
            o->props["$length"] = Value::intg(n);
            return Value::object(o);
        };
        c->classDef->instanceMembers.push_back(cl);
        // currentLabel：当前帧所在（或之前最近的）帧标签名；AS3 无标签时为 ""。
        Member cLabel; cLabel.kind = Member::Getter; cLabel.name = "currentLabel";
        cLabel.native = [this](std::shared_ptr<Object> self,
                               const std::vector<Value>&) -> Value {
            const std::map<std::string, int>* labels = &movie_.frameLabels;
            if (self && self->display && self->display->characterId != 0) {
                auto it = movie_.characters.find(self->display->characterId);
                if (it != movie_.characters.end() && it->second &&
                    it->second->kind == Character::Kind::Sprite &&
                    !it->second->frameLabels.empty())
                    labels = &it->second->frameLabels;
            }
            const int cur = (self && self->display) ? self->display->currentFrame + 1 : 1;
            std::string best;
            int bestFrame = -1;
            for (const auto& kv : *labels) {
                if (kv.second <= cur && kv.second > bestFrame) {
                    best = kv.first;
                    bestFrame = kv.second;
                }
            }
            return Value::str(best);
        };
        c->classDef->instanceMembers.push_back(cLabel);
        Member gap; gap.kind = Member::Method; gap.name = "gotoAndPlay";
        gap.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::undefined();
            auto d = self->display;
            int idx = resolveFrameIndex(d, a[0]);
            d->currentFrame = idx;
            d->playing = true;
            applyTimelineFrame(d, idx);
            if (std::getenv("SWF_AS_DEBUG")) {
                auto ci = movie_.characters.find(d->characterId);
                int total = (d->characterId == 0) ? static_cast<int>(movie_.frames.size())
                          : (ci != movie_.characters.end() && ci->second ? static_cast<int>(ci->second->spriteFrames.size()) : 0);
                std::fprintf(stderr, "[gotoPlay] name=%s cid=%d total=%d -> %d fs=%d\n",
                             d->name.c_str(), d->characterId, total, idx,
                             (int)d->frameScripts.count(idx));
            }
            // Flash 语义：goto 到目标帧后立即执行该帧的 addFrameScript
            // （若脚本又重新 goto 到同一帧则不重复执行，避免自递归）
            bool reenter = (d->lastScriptFrame == idx);
            d->lastScriptFrame = idx;
            auto fs = d->frameScripts.find(idx);
            if (!reenter && fs != d->frameScripts.end() && fs->second.kind == VKind::Object && fs->second.obj)
                invokeValue(fs->second, {}, self);
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(gap);
        Member gas; gas.kind = Member::Method; gas.name = "gotoAndStop";
        gas.native = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self || !self->display || a.empty()) return Value::undefined();
            auto d = self->display;
            int idx = resolveFrameIndex(d, a[0]);
            d->currentFrame = idx;
            d->playing = false;
            applyTimelineFrame(d, idx);
            if (std::getenv("SWF_AS_DEBUG")) {
                auto ci = movie_.characters.find(d->characterId);
                int total = (d->characterId == 0) ? static_cast<int>(movie_.frames.size())
                          : (ci != movie_.characters.end() && ci->second ? static_cast<int>(ci->second->spriteFrames.size()) : 0);
                std::fprintf(stderr, "[gotoStop] name=%s cid=%d total=%d -> %d fs=%d keys=",
                             d->name.c_str(), d->characterId, total, idx,
                             (int)d->frameScripts.count(idx));
                for (const auto& kv : d->frameScripts) std::fprintf(stderr, "%d,", kv.first);
                std::fprintf(stderr, "\n");
            }
            // Flash 语义：goto 到目标帧后立即执行该帧的 addFrameScript
            // （若脚本又重新 goto 到同一帧则不重复执行，避免自递归）
            bool reenter = (d->lastScriptFrame == idx);
            d->lastScriptFrame = idx;
            auto fs = d->frameScripts.find(idx);
            if (!reenter && fs != d->frameScripts.end() && fs->second.kind == VKind::Object && fs->second.obj)
                invokeValue(fs->second, {}, self);
            return Value::undefined();
        };
        c->classDef->instanceMembers.push_back(gas);
    }
    {
        auto c = mkClass("TextField", ObjKind::TextField);
        inheritNative(c->classDef, "DisplayObject");
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
        auto cd = c->classDef;
        // new Timer(delay, repeatCount = 0)
        cd->ctor = [this](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
            if (!self) return Value::undefined();
            TimerObjectTask t;
            t.obj = self;
            t.delayMs = a.empty() ? 0.0 : a[0].toNumber();
            t.repeatCount = a.size() > 1 ? a[1].toInt() : 0;
            t.dueMs = timerNowMs_ + t.delayMs;
            timerObjects_.push_back(t);
            self->props["delay"] = Value::num(t.delayMs);
            self->props["repeatCount"] = Value::intg(t.repeatCount);
            return Value::undefined();
        };
        auto findTask = [this](const std::shared_ptr<Object>& self) -> TimerObjectTask* {
            for (auto& t : timerObjects_) {
                auto o = t.obj.lock();
                if (o && o.get() == self.get()) return &t;
            }
            return nullptr;
        };
        auto add = [&](const char* n, std::function<Value(const std::shared_ptr<Object>&,
                                                          const std::vector<Value>&)> fn) {
            Member m; m.kind = Member::Method; m.name = n;
            m.native = [fn](std::shared_ptr<Object> self, const std::vector<Value>& a) -> Value {
                return fn(self, a);
            };
            cd->instanceMembers.push_back(m);
        };
        add("start", [this, findTask](const std::shared_ptr<Object>& self, const std::vector<Value>&) {
            auto* t = findTask(self);
            if (t) {
                t->currentCount = 0;
                t->running = true;
                t->dueMs = timerNowMs_ + (t->delayMs > 0 ? t->delayMs : 1.0);
            }
            return Value::undefined();
        });
        add("stop", [this, findTask](const std::shared_ptr<Object>& self, const std::vector<Value>&) {
            auto* t = findTask(self);
            if (t) t->running = false;
            return Value::undefined();
        });
        add("reset", [this, findTask](const std::shared_ptr<Object>& self, const std::vector<Value>&) {
            auto* t = findTask(self);
            if (t) { t->currentCount = 0; t->running = false; }
            return Value::undefined();
        });
        for (const char* n : {"delay", "repeatCount", "currentCount", "running"}) {
            Member g; g.kind = Member::Getter; g.name = n;
            g.native = [this, findTask, n](std::shared_ptr<Object> self,
                                           const std::vector<Value>&) -> Value {
                auto* t = self ? findTask(self) : nullptr;
                if (!t) return Value::num(0);
                if (std::string(n) == "delay") return Value::num(t->delayMs);
                if (std::string(n) == "repeatCount") return Value::intg(t->repeatCount);
                if (std::string(n) == "currentCount") return Value::intg(t->currentCount);
                return Value::boolean(t->running);
            };
            cd->instanceMembers.push_back(g);
        }
    }
    {
        // ---- Graphics：绘制命令记录（渲染端 Render.cpp renderGraphicsInto 回放）----
        // 此前整组方法为 no-op，AVM2 游戏经 graphics 动态绘制的内容完全不可见。
        auto c = mkClass("Graphics", ObjKind::Graphics);
        auto cmdsOf = [](const std::shared_ptr<Object>& self)
                -> std::shared_ptr<std::vector<GraphicsCmd>> {
            auto d = self ? self->gfxTarget.lock() : nullptr;
            if (!d) return nullptr;
            if (!d->graphics) d->graphics = std::make_shared<std::vector<GraphicsCmd>>();
            return d->graphics;
        };
        auto argN = [](const std::vector<Value>& a, std::size_t i) -> double {
            return i < a.size() ? a[i].toNumber() : 0.0;
        };
        // AS3 颜色为 0xRRGGBB + 独立 alpha(0..1)；命令内统一存 ARGB（高字节 alpha）。
        auto argbOf = [](const std::vector<Value>& a, std::size_t ci, std::size_t ai) -> std::uint32_t {
            const std::uint32_t rgb = (ci < a.size() ? a[ci].toUInt() : 0u) & 0xFFFFFFu;
            const double al = ai < a.size() ? a[ai].toNumber() : 1.0;
            const std::uint32_t av = al <= 0.0 ? 0u : al >= 1.0 ? 255u
                                      : static_cast<std::uint32_t>(al * 255.0 + 0.5);
            return (av << 24) | rgb;
        };
        auto push = [cmdsOf](const std::shared_ptr<Object>& self, GraphicsCmd::Op op)
                -> std::shared_ptr<std::vector<GraphicsCmd>> {
            auto v = cmdsOf(self);
            if (v) {
                GraphicsCmd m;
                m.op = op;
                v->push_back(m);
            }
            return v;
        };
        auto member = [c](const char* n, NativeFn fn) {
            Member m;
            m.kind = Member::Method;
            m.name = n;
            m.native = std::move(fn);
            c->classDef->instanceMembers.push_back(m);
        };
        member("beginFill", [push, argbOf](std::shared_ptr<Object> self,
                                           const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::BeginFill)) v->back().color = argbOf(a, 0, 1);
            return Value::undefined();
        });
        // beginGradientFill 最小近似：取第一个色停做纯色填充（渐变渲染待接入）。
        member("beginGradientFill", [push, argbOf](std::shared_ptr<Object> self,
                                                   const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::BeginFill)) {
                std::uint32_t color = 0xFF808080;
                if (a.size() > 1 && a[1].kind == VKind::Object && a[1].obj) {
                    auto it = a[1].obj->props.find("0");
                    if (it != a[1].obj->props.end()) {
                        std::vector<Value> one{it->second, Value::num(1.0)};
                        color = argbOf(one, 0, 1);
                    }
                }
                v->back().color = color;
            }
            return Value::undefined();
        });
        // beginBitmapFill / lineGradientStyle 暂不支持（无害 no-op）。
        member("beginBitmapFill", [](std::shared_ptr<Object>,
                                     const std::vector<Value>&) -> Value {
            return Value::undefined();
        });
        member("lineGradientStyle", [](std::shared_ptr<Object>,
                                       const std::vector<Value>&) -> Value {
            return Value::undefined();
        });
        member("endFill", [push](std::shared_ptr<Object> self,
                                 const std::vector<Value>&) -> Value {
            push(self, GraphicsCmd::Op::EndFill);
            return Value::undefined();
        });
        member("lineStyle", [push, argN, argbOf](std::shared_ptr<Object> self,
                                                 const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::LineStyle)) {
                const double t = argN(a, 0);
                v->back().a = (t == t) ? t : 0.0;   // NaN（省略参数）= 关闭描边
                v->back().color = argbOf(a, 1, 2);
            }
            return Value::undefined();
        });
        member("moveTo", [push, argN](std::shared_ptr<Object> self,
                                      const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::MoveTo)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
            }
            return Value::undefined();
        });
        member("lineTo", [push, argN](std::shared_ptr<Object> self,
                                      const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::LineTo)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
            }
            return Value::undefined();
        });
        member("curveTo", [push, argN](std::shared_ptr<Object> self,
                                       const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::CurveTo)) {
                v->back().cx = argN(a, 0);
                v->back().cy = argN(a, 1);
                v->back().x = argN(a, 2);
                v->back().y = argN(a, 3);
            }
            return Value::undefined();
        });
        member("drawRect", [push, argN](std::shared_ptr<Object> self,
                                        const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::Rect)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
                v->back().w = argN(a, 2);
                v->back().h = argN(a, 3);
            }
            return Value::undefined();
        });
        member("drawRoundRect", [push, argN](std::shared_ptr<Object> self,
                                             const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::RoundRect)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
                v->back().w = argN(a, 2);
                v->back().h = argN(a, 3);
                const double ew = argN(a, 4), eh = argN(a, 5);
                v->back().r = std::min(ew, eh) * 0.5;
            }
            return Value::undefined();
        });
        member("drawCircle", [push, argN](std::shared_ptr<Object> self,
                                          const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::Circle)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
                v->back().r = argN(a, 2);
            }
            return Value::undefined();
        });
        member("drawEllipse", [push, argN](std::shared_ptr<Object> self,
                                           const std::vector<Value>& a) -> Value {
            if (auto v = push(self, GraphicsCmd::Op::Ellipse)) {
                v->back().x = argN(a, 0);
                v->back().y = argN(a, 1);
                v->back().w = argN(a, 2);
                v->back().h = argN(a, 3);
            }
            return Value::undefined();
        });
        member("clear", [cmdsOf](std::shared_ptr<Object> self,
                                 const std::vector<Value>&) -> Value {
            if (auto v = cmdsOf(self)) v->clear();
            return Value::undefined();
        });
    }

    // Stage 注册早于 DisplayObject，此处补继承链（获得 addEventListener / mouseX 等成员）
    {
        auto st = globals_->props.find("Stage");
        auto dobj = globals_->props.find("DisplayObject");
        if (st != globals_->props.end() && dobj != globals_->props.end() &&
            st->second.kind == VKind::Object && st->second.obj && st->second.obj->classDef &&
            dobj->second.kind == VKind::Object && dobj->second.obj && dobj->second.obj->classDef)
            st->second.obj->classDef->superClass = dobj->second.obj->classDef;
    }
}

// ---- 文档类实例化 ----
void Vm::instantiateDocumentClasses() {
    const bool dbg = std::getenv("SWF_AS_DEBUG") != nullptr;
    std::vector<std::shared_ptr<DisplayObject>> created;
    bool hasRoot = false;
    // 临时诊断：dump 元件137_112 (cid 1280) 精灵时间轴各帧具名放置
    if (dbg) {
        auto it1280 = movie_.characters.find(1280);
        if (it1280 != movie_.characters.end() && it1280->second &&
            it1280->second->kind == Character::Kind::Sprite) {
            auto& sf = it1280->second->spriteFrames;
            std::fprintf(stderr, "[as] dump 1280 frames=%zu\n", sf.size());
            for (const auto& kv : it1280->second->frameLabels)
                std::fprintf(stderr, "[as] 1280 label '%s' -> %d\n", kv.first.c_str(), kv.second);
            for (size_t fi = 0; fi < sf.size(); ++fi)
                std::fprintf(stderr, "[as] 1280 f%zu items=%zu acts=%zu\n", fi, sf[fi].items.size(),
                             sf[fi].doActions.size());
            for (size_t fi = 0; fi < sf.size(); ++fi) {
                for (const auto& di : sf[fi].items)
                    if (!di.name.empty())
                        std::fprintf(stderr, "[as] 1280 f%zu name='%s' cid=%u x=%.0f y=%.0f\n", fi,
                                     di.name.c_str(), (unsigned)di.characterId,
                                     di.matrix.tx / 20.0, di.matrix.ty / 20.0);
            }
        }
        // Show 精灵 (cid 1185) 各帧结构与具名放置
        auto itShow = movie_.characters.find(1185);
        if (itShow != movie_.characters.end() && itShow->second &&
            itShow->second->kind == Character::Kind::Sprite) {
            auto& sf = itShow->second->spriteFrames;
            std::fprintf(stderr, "[as] dump 1185 frames=%zu\n", sf.size());
            for (size_t fi = 0; fi < sf.size(); ++fi) {
                std::fprintf(stderr, "[as] 1185 f%zu items=%zu actions=%zu\n", fi, sf[fi].items.size(),
                             sf[fi].doActions.size());
                for (const auto& di : sf[fi].items)
                    if (!di.name.empty())
                        std::fprintf(stderr, "[as] 1185 f%zu name='%s' cid=%u\n", fi, di.name.c_str(),
                                     (unsigned)di.characterId);
            }
        }
        // LV 子精灵各帧结构
        for (uint16_t lv : {1184, 1174, 1165, 1156}) {
            auto it = movie_.characters.find(lv);
            if (it == movie_.characters.end() || !it->second ||
                it->second->kind != Character::Kind::Sprite) continue;
            auto& sf = it->second->spriteFrames;
            std::fprintf(stderr, "[as] dump %u frames=%zu\n", lv, sf.size());
            for (size_t fi = 0; fi < sf.size(); ++fi) {
                std::string names;
                for (const auto& di : sf[fi].items)
                    if (!di.name.empty()) names += di.name + "(" + std::to_string(di.characterId) + ") ";
                std::fprintf(stderr, "[as] %u f%zu items=%zu acts=%zu named=[%s]\n", lv, fi,
                             sf[fi].items.size(), sf[fi].doActions.size(), names.c_str());
            }
        }
        // 主时间轴各帧具名放置
        std::fprintf(stderr, "[as] dump root frames=%zu\n", movie_.frames.size());
        for (size_t fi = 0; fi < movie_.frames.size(); ++fi)
            for (const auto& di : movie_.frames[fi].items)
                if (!di.name.empty())
                    std::fprintf(stderr, "[as] root f%zu name='%s' cid=%u\n", fi, di.name.c_str(),
                                 (unsigned)di.characterId);
    }
    // 角色 id -> AS3 类名（SymbolClass 反向映射），供时间轴实例选择具体类
    classByCid_.clear();
    for (const auto& kv : movie_.symbolClasses)
        if (!classByCid_.count(kv.second)) classByCid_[kv.second] = kv.first;

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
        if (dbg) std::fprintf(stderr, "[as] instantiate %s cid=%u iinit=%u\n", cname.c_str(), cid,
                              abc_.instances[ii].iinit);

        // 执行实例构造函数
        if (ii < static_cast<int>(abc_.instances.size())) {
            uint32_t init = abc_.instances[ii].iinit;
            if (init < abc_.methods.size()) {
                int bi = abc_.methods[init].bodyIndex;
                if (bi >= 0 && bi < static_cast<int>(abc_.methodBodies.size()))
                    exec(abc_.methodBodies[bi], {}, inst);
            }
        }
        created.push_back(inst->display);
        if (cid == 0 && !hasRoot) {
            hasRoot = true;
            // 根时间轴（主时间轴）实例：按第 1 帧的放置列表建立 AS3 显示子对象，
            // 这样 getChildByName("main"/"menu"...) 才能命中时间轴上的实例。
            if (inst->display && !movie_.frames.empty()) {
                auto parent = inst->display;
                // 按 Flash 语义从第 1 帧（索引 0）开始建根：静态渲染首帧时
                // （setInitialFrame(0)）同样取第 0 帧，不把后续帧的游戏内容提前画出来。
                // 后续内容由根时间轴推进 + gotoAndStop 驱动。
                size_t rootFrame = initialFrame_ >= 0 ? static_cast<size_t>(initialFrame_) : 0;
                // 主时间轴仍从第 1 帧（索引 0）开始播放，保证 addFrameScript(0) 的入口逻辑执行
                if (dbg) std::fprintf(stderr, "[as] root frame=%zu items=%zu\n", rootFrame,
                                      movie_.frames[rootFrame].items.size());
                for (const auto& di : movie_.frames[rootFrame].items) {
                    if (!di.characterId) continue;
                    std::shared_ptr<Object> child;
                    auto ci = classByCid_.find(di.characterId);
                    if (ci != classByCid_.end()) {
                        auto git = globals_->props.find(ci->second);
                        if (git == globals_->props.end() || git->second.kind != VKind::Object) {
                            size_t p = ci->second.rfind('.');
                            if (p != std::string::npos) git = globals_->props.find(ci->second.substr(p + 1));
                        }
                        if (git != globals_->props.end() && git->second.kind == VKind::Object && git->second.obj &&
                            git->second.obj->instanceIndex >= 0)
                            child = constructObject(git->second, {}).obj;
                    }
                    ObjKind ck = ObjKind::MovieClip;
                    if (di.character) {
                        if (di.character->kind == Character::Kind::Shape) ck = ObjKind::Shape;
                        else if (di.character->kind == Character::Kind::Text ||
                                 di.character->kind == Character::Kind::EditText) ck = ObjKind::TextField;
                    }
                    if (!child) child = newObject(ck, "MovieClip");
                    if (!child->display) {
                        child->display = std::make_shared<DisplayObject>();
                        child->display->owner = child;
                    }
                    auto d = child->display;
                    d->characterId = di.characterId;
                    d->name = di.name;
                    d->depth = static_cast<int>(di.depth);
                    // PlaceObject3 的混合模式与滤镜列表：与静态显示列表等价，
                    // 运行时树此前完全丢弃这两项，导致发光/投影与混合叠加丢失。
                    d->blend = di.blend;
                    if (!di.filters.empty())
                        d->filters = std::make_shared<std::vector<Filter>>(di.filters);
                    d->x = di.matrix.tx / 20.0;
                    d->y = di.matrix.ty / 20.0;
                    // 矩阵 [sx r0; r1 sy] → scaleX/scaleY/rotation（与渲染器分解方式一致）
                    double cxs = di.matrix.sx, cys = di.matrix.sy;
                    d->scaleX = std::sqrt(cxs * cxs + static_cast<double>(di.matrix.r1) * di.matrix.r1);
                    if (d->scaleX == 0) d->scaleX = 1;
                    // scaleY 取带符号值 det/scaleX：镜像放置（det<0，如娃娃左眼虹膜）时
                    // sqrt 取绝对值会丢符号，重建后镜像变成 180° 旋转，图形翻到错误一侧。
                    d->scaleY = (cxs * cys - static_cast<double>(di.matrix.r0) * di.matrix.r1) / d->scaleX;
                    if (d->scaleY == 0) d->scaleY = 1;
                    d->rotation = std::atan2(static_cast<double>(di.matrix.r1), cxs) * 180.0 / 3.141592653589793;
                    // 完整 2x2 线性部分（无量纲比例，与渲染器 Lt 的 sx/r0/r1/sy 约定一致；
                    // 平移仍由 x/y（像素）承载）。渲染器直接用它，保留错切/旋转的非正交分量。
                    // scaleX/scaleY/rotation 只是 3 自由度近似，对带错切的矩阵（如浴垫粉色圆点
                    // 770）会丢分量导致图形错位。
                    d->hasFull = true;
                    d->fullA = cxs;            d->fullB = di.matrix.r0;
                    d->fullC = di.matrix.r1;    d->fullD = cys;
                    if (di.cxform.hasMult) d->alpha = di.cxform.ma;
                    // 完整 ColorTransform：rgb 乘/加项（ma 已并入 alpha，aa 罕见忽略）
                    d->cxMr = di.cxform.mr; d->cxMg = di.cxform.mg; d->cxMb = di.cxform.mb;
                    d->cxAr = di.cxform.ar; d->cxAg = di.cxform.ag; d->cxAb = di.cxform.ab;
                    d->parent = parent;
                    parent->children.push_back(d);
                    created.push_back(d);
                }
                // 递归构建该帧内嵌套精灵的内部帧内容（复用上面的子实例，不重复创建）
                applyTimelineFrame(parent, static_cast<int>(rootFrame));
            }
        }
    }
    if (hasRoot) {
        // 仅文档类（cid=0）作为舞台根；其余 SymbolClass 只是类定义，由时间轴/代码按需实例化
        for (auto& d : created)
            if (!d->parent && d->characterId == 0) stage.push_back(d);
    } else {
        for (auto& d : created)
            if (!d->parent) stage.push_back(d);
    }
}

// 时间轴放置记录的变换指纹。累积快照里静止对象每帧都会出现同一个 item，
// 用它区分「本帧有新的 Place/Move」和「只是沿用上一帧的放置」。
static std::string timelineTransformStamp(const DisplayItem& di) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.3f|%.3f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f|%.6f",
                  di.matrix.tx / 20.0, di.matrix.ty / 20.0,
                  static_cast<double>(di.matrix.sx), static_cast<double>(di.matrix.sy),
                  static_cast<double>(di.matrix.r0), static_cast<double>(di.matrix.r1),
                  static_cast<double>(di.cxform.ma), static_cast<double>(di.cxform.mr),
                  static_cast<double>(di.cxform.mg), static_cast<double>(di.cxform.mb));
    return buf;
}

// 按时间轴某帧（0 基）重建显示对象的子实例。
// SWF 中每帧的 items 是 ShowFrame 时的累积放置列表快照（按 depth 排序），
// 因此这里以 depth 为键复用实例：同一 depth 同一角色则只更新变换/名字，
// 角色被替换时重建 AS3 对象，快照中已不存在的 depth 从显示树移除。
// AS3 代码 addChild 创建的子树（depth < 0）保持不动、放在时间轴内容之后。
void Vm::applyTimelineFrame(const std::shared_ptr<DisplayObject>& d, int frameIndex) {
    if (!d) return;
    const std::vector<Frame>* frames = nullptr;
    if (d->characterId == 0) {
        frames = &movie_.frames;
    } else {
        auto it = movie_.characters.find(d->characterId);
        if (it != movie_.characters.end() && it->second &&
            it->second->kind == Character::Kind::Sprite)
            frames = &it->second->spriteFrames;
    }
    if (!frames || frames->empty()) return;
    if (frameIndex < 0) frameIndex = 0;
    if (frameIndex >= static_cast<int>(frames->size()))
        frameIndex = static_cast<int>(frames->size()) - 1;
    const Frame& frame = (*frames)[frameIndex];
    if (std::getenv("SWF_TL_DBG") && d->characterId == 386) {
        static int _n = 0;
        if (_n < 50) {
            std::fprintf(stderr, "[tl386] f=%d items=%d:", frameIndex, (int)frame.items.size());
            for (auto& x : frame.items) std::fprintf(stderr, " %d:%d", x.depth, x.characterId);
            std::fprintf(stderr, "\n"); ++_n;
        }
    }
    if (std::getenv("SWF_TL_DBG") && d->characterId == 378) {
        static int _n = 0;
        if (_n < 40) {
            std::fprintf(stderr, "[tl378] f=%d:", frameIndex);
            for (auto& x : frame.items) std::fprintf(stderr, " %d:%d", x.depth, x.characterId);
            std::fprintf(stderr, "\n"); ++_n;
        }
    }

    std::map<int, std::shared_ptr<DisplayObject>> existing;
    // 记录 AS3 动态子对象在重建前的绝对索引。此前每帧都把它们整体追加到
    // 时间轴内容末尾，导致 addChildAt / setChildIndex / swapChildren 设定的
    // 层级在下一帧被重置。按原索引插回即可跨帧保持脚本设定的显示顺序。
    std::vector<std::pair<int, std::shared_ptr<DisplayObject>>> codePositions;
    for (std::size_t k = 0; k < d->children.size(); ++k) {
        const auto& c = d->children[k];
        if (!c) continue;
        if (c->depth >= 0) existing[c->depth] = c;
        else codePositions.emplace_back(static_cast<int>(k), c);
    }

    std::vector<std::shared_ptr<DisplayObject>> out;
    for (const auto& di : frame.items) {
        const int depth = static_cast<int>(di.depth);
        std::shared_ptr<DisplayObject> child;
        auto e = existing.find(depth);
        if (e != existing.end() && e->second->characterId == di.characterId && di.characterId != 0) {
            // 同一角色复用：沿用已存在实例
            if (std::getenv("SWF_TL_DBG") && d->characterId == 386 && di.characterId == 378 &&
                depth == 9 && (frameIndex == 14 || frameIndex == 100))
                std::fprintf(stderr, "[reuse386] f=%d depth=%d tx=%.0f ty=%.0f\n",
                             frameIndex, depth, di.matrix.tx, di.matrix.ty);
            child = e->second;
            existing.erase(e);
        } else if (e != existing.end() && di.characterId == 0) {
            // Modify 记录（PlaceObject 不带 characterId，仅携带新矩阵/属性）：沿用
            // 已存在实例，仅更新其变换。这是逐帧位移动画（飘落装饰、入场补间、
            // 气泡标签动效）的驱动方式；之前被顶层 if(!di.characterId) continue 跳过，
            // 导致所有 Move 驱动的动画静止在初始帧。
            child = e->second;
            existing.erase(e);
        } else if (di.characterId != 0) {
            // 新建时间轴子实例：优先用 SymbolClass 关联的 AS3 类（执行其构造函数）
            std::shared_ptr<Object> obj;
            auto ci = classByCid_.find(di.characterId);
            if (ci != classByCid_.end() && globals_) {
                auto git = globals_->props.find(ci->second);
                if (git == globals_->props.end() || git->second.kind != VKind::Object) {
                    size_t p = ci->second.rfind('.');
                    if (p != std::string::npos) git = globals_->props.find(ci->second.substr(p + 1));
                }
                if (git != globals_->props.end() && git->second.kind == VKind::Object &&
                    git->second.obj && git->second.obj->instanceIndex >= 0) {
                    auto v = constructObject(git->second, {});
                    if (v.kind == VKind::Object) obj = v.obj;
                }
            }
            if (!obj) {
                ObjKind ck = ObjKind::MovieClip;
                std::string cname = "MovieClip";
                if (di.character) {
                    if (di.character->kind == Character::Kind::Shape) { ck = ObjKind::Shape; cname = "Shape"; }
                    else if (di.character->kind == Character::Kind::Text ||
                             di.character->kind == Character::Kind::EditText) {
                        ck = ObjKind::TextField; cname = "TextField";
                    } else if (di.character->kind == Character::Kind::Bitmap) { ck = ObjKind::Bitmap; cname = "Bitmap"; }
                }
                obj = newObject(ck, cname);
                // 无 SymbolClass 关联的时间轴子实例仍要能调用 DisplayObject/EventDispatcher 原生方法
                // （如 pp1.addEventListener）；按类名挂接内建类定义。
                if (globals_ && cname != "Shape" && cname != "Bitmap") {
                    auto git = globals_->props.find(cname);
                    if (git != globals_->props.end() && git->second.kind == VKind::Object && git->second.obj)
                        obj->cls = git->second.obj->classDef;
                }
            }
            if (!obj->display) {
                obj->display = std::make_shared<DisplayObject>();
                obj->display->owner = obj;
            }
            child = obj->display;
            child->characterId = di.characterId;
            child->depth = depth;
            child->parent = d;
            // 新建实例时带入 PlaceObject3 的 blend / filters；复用实例不覆盖，
            // 因为 Move 记录不带这些字段（Flash 语义是沿用上一次放置的值）。
            child->blend = di.blend;
            if (!di.filters.empty())
                child->filters = std::make_shared<std::vector<Filter>>(di.filters);
        } else {
            continue;  // 孤立 modify（无 character 且 depth 不在当前显示树），跳过
        }
        child->depth = depth;
        child->parent = d;
        child->clipDepth = di.clipDepth;
        child->ratio = di.ratio;
        if (!di.name.empty()) child->name = di.name;
        // 脚本/时间轴变换优先级：脚本写过几何属性后，只有时间轴本帧产生了
        // 新的变换（Place/Move）才重新接管；静止的累积放置记录不再冲刷脚本值。
        const std::string stamp = timelineTransformStamp(di);
        const bool keepScript = child->scriptTransform &&
                                !child->tlStamp.empty() && child->tlStamp == stamp;
        if (!keepScript) {
            child->x = di.matrix.tx / 20.0;
            child->y = di.matrix.ty / 20.0;
            // 矩阵 [sx r0; r1 sy] → scaleX/scaleY/rotation（与渲染器分解方式一致）
            double cxs = di.matrix.sx, cys = di.matrix.sy;
            child->scaleX = std::sqrt(cxs * cxs + static_cast<double>(di.matrix.r1) * di.matrix.r1);
            if (child->scaleX == 0) child->scaleX = 1;
            // scaleY 取带符号值 det/scaleX（同上：保留镜像符号）
            child->scaleY = (cxs * cys - static_cast<double>(di.matrix.r0) * di.matrix.r1) / child->scaleX;
            if (child->scaleY == 0) child->scaleY = 1;
            child->rotation = std::atan2(static_cast<double>(di.matrix.r1), cxs) * 180.0 / 3.141592653589793;
            child->alpha = di.cxform.hasMult ? di.cxform.ma : 1.0;
            // 完整 ColorTransform：rgb 乘/加项（ma 已并入 alpha，aa 罕见忽略）
            child->cxMr = di.cxform.mr; child->cxMg = di.cxform.mg; child->cxMb = di.cxform.mb;
            child->cxAr = di.cxform.ar; child->cxAg = di.cxform.ag; child->cxAb = di.cxform.ab;
            child->tlStamp = stamp;
            child->scriptTransform = false;
        }
        // 嵌套精灵：首次进入显示树时构建其第 1 帧内容
        if (!child->timelineBuilt) applyTimelineFrame(child, child->currentFrame);
        out.push_back(child);
    }
    // 依次按记录索引插入：插入本身会把后续元素右移，因此当时间轴子对象数量
    // 不变时能精确还原原顺序；数量变化时索引 clamp 到末尾（仍保持相对顺序）。
    for (const auto& cp : codePositions) {
        int idx = cp.first;
        if (idx < 0) idx = 0;
        if (idx > static_cast<int>(out.size())) idx = static_cast<int>(out.size());
        out.insert(out.begin() + idx, cp.second);
    }
    // 本帧已不存在的旧时间轴子实例：从显示树脱离（AS3 中的引用仍可继续使用）
    for (auto& kv : existing)
        if (kv.second) kv.second->parent.reset();
    if (std::getenv("SWF_TL_DBG") && d->characterId == 386) {
        static int _m = 0;
        if (_m < 2 && (frameIndex == 20 || frameIndex == 30)) {
            std::function<void(const std::shared_ptr<DisplayObject>&, int)> dumpTree =
                [&](const std::shared_ptr<DisplayObject>& c, int lv) {
                    if (!c || lv > 6) return;
                    std::fprintf(stderr, "%*s cid=%-5d depth=%-4d a=%.2f kids=%zu cf=%-3d built=%d vis=%d clip=%u x=%.0f y=%.0f sx=%.2f sy=%.2f\n",
                                 lv * 2, "", c->characterId, c->depth, c->alpha,
                                 c->children.size(), c->currentFrame, (int)c->timelineBuilt,
                                 (int)c->visible, c->clipDepth, c->x, c->y, c->scaleX, c->scaleY);
                    for (auto& k : c->children) dumpTree(k, lv + 1);
                };
            for (auto& c : out) {
                if (c->depth == 9) {
                    std::fprintf(stderr, "[tree] f=%d depth9:\n", frameIndex);
                    dumpTree(c, 0);
                }
            }
            ++_m;
        }
    }
    d->children.swap(out);
    d->timelineBuilt = true;

}

// gotoAndStop 语义：先改播放头，再按目标帧重建放置列表。
void Vm::setClipFrame(const std::shared_ptr<DisplayObject>& d, int frameIndex) {
    if (!d) return;
    d->currentFrame = frameIndex;
    applyTimelineFrame(d, frameIndex);
}

// gotoAndStop/gotoAndPlay 的帧参数：字符串先按帧标签，再按数字；数字为 AS3 的 1 基帧号。
int Vm::resolveFrameIndex(const std::shared_ptr<DisplayObject>& d, const Value& v) const {
    if (!d) return 0;
    int total = 0;
    if (d->characterId == 0) {
        total = static_cast<int>(movie_.frames.size());
    } else {
        auto it = movie_.characters.find(d->characterId);
        if (it != movie_.characters.end() && it->second &&
            it->second->kind == Character::Kind::Sprite)
            total = static_cast<int>(it->second->spriteFrames.size());
    }
    // 文档类实例底层是某个 sprite 定义（characterId != 0），但它的时间轴就是主
    // SWF 时间轴，帧标签应优先从主时间轴解析；找不到再回退到自身 sprite 标签。
    // 否则 gotoAndPlay("startFrame") 等会在文档类上失败，游戏主流程无法前进。
    auto resolveLabel = [&](const std::string& name) -> int {
        auto m = movie_.frameLabels.find(name);
        if (m != movie_.frameLabels.end()) return m->second - 1;
        if (d->characterId != 0) {
            auto ci = movie_.characters.find(d->characterId);
            if (ci != movie_.characters.end() && ci->second &&
                ci->second->kind == Character::Kind::Sprite) {
                auto s = ci->second->frameLabels.find(name);
                if (s != ci->second->frameLabels.end()) return s->second - 1;
            }
        }
        return -1;
    };
    if (v.kind == VKind::String) {
        int r = resolveLabel(v.s);
        if (r >= 0) {
            if (total > 0 && r >= total) r = total - 1;
            return r;
        }
        char* endp = nullptr;
        long n = std::strtol(v.s.c_str(), &endp, 10);
        if (endp == v.s.c_str()) return total > 0 ? 0 : 0;
        int idx = static_cast<int>(n) - 1;
        if (idx < 0) idx = 0;
        if (total > 0 && idx >= total) idx = total - 1;
        return idx;
    }
    int idx = v.toInt() - 1;
    if (idx < 0) idx = 0;
    if (total > 0 && idx >= total) idx = total - 1;
    return idx;
}

void Vm::run() {
    // SWF_AS_DIS=<mi>[,<mi>...]：静态反汇编指定方法（不需要该方法被执行）。
    // traceOps 只能看到实际走过的路径；判断"某段逻辑为何没跑"必须看全量字节码，
    // 包括条件跳转后未进入的分支。支持逗号分隔的多个 methodIndex。
    if (const char* disEnv = std::getenv("SWF_AS_DIS")) {
        std::vector<uint32_t> targets;
        std::string spec(disEnv);
        for (size_t p = 0; p < spec.size();) {
            size_t q = spec.find(',', p);
            if (q == std::string::npos) q = spec.size();
            if (q > p) {
                std::string tok = spec.substr(p, q - p);
                bool numeric = !tok.empty() &&
                    tok.find_first_not_of("0123456789") == std::string::npos;
                if (numeric) {
                    targets.push_back(static_cast<uint32_t>(std::atoi(tok.c_str())));
                } else {
                    // 也接受 AS3 方法名（精确匹配），便于反汇编帧脚本 frameN。
                    for (size_t m = 0; m < abc_.methods.size(); ++m)
                        if (abc_.methods[m].name == tok)
                            targets.push_back(static_cast<uint32_t>(m));
                }
            }
            p = q + 1;
        }
        for (uint32_t want : targets) {
            for (const auto& body : abc_.methodBodies) {
                if (body.methodIndex != want) continue;
                std::fprintf(stderr, "[dis] method=%u code=%zu\n", want, body.code.size());
                for (size_t pc = 0; pc < body.code.size();) {
                    avm2::Instruction ins = avm2::decode(body.code.data(), body.code.size(), pc);
                    if (ins.size == 0) break;
                    const char* nm = ins.info && ins.info->valid() ? ins.info->name : "db";
                    std::string out = "[" + std::to_string(pc) + "] " + nm;
                    if (ins.info && ins.info->nameOperand != avm2::kNoOperand &&
                        ins.info->nameOperand < ins.operands.size()) {
                        uint32_t mn = static_cast<uint32_t>(ins.operands[ins.info->nameOperand]);
                        std::string lbl = (mn < abc_.multinames.size())
                            ? abc_.str(abc_.multinames[mn].name) : "?";
                        out += " [" + lbl + "]";
                    }
                    for (size_t k = 0; k < ins.operands.size(); ++k)
                        out += " " + std::to_string(ins.operands[k]);
                    // pushstring 只给字符串池索引，附上实际文本便于读
                    // getChildByName("...") / clip["..."] 一类的实例名。
                    if (std::string(nm) == "pushstring" && !ins.operands.empty()) {
                        uint32_t si = static_cast<uint32_t>(ins.operands[0]);
                        out += " str=\"" + abc_.str(si) + "\"";
                    }
                    std::fprintf(stderr, "[dis] %s\n", out.c_str());
                    pc += ins.size;
                }
            }
        }
    }
    // SWF_AS_FIND=<子串>：静态扫描所有方法体，列出引用该 multiname 的指令。
    // 用于回答"某个 AS3 方法究竟被谁调用"，不依赖运行时是否真的走到。
    if (const char* findEnv = std::getenv("SWF_AS_FIND")) {
        std::string needle(findEnv);
        for (const auto& body : abc_.methodBodies) {
            for (size_t pc = 0; pc < body.code.size();) {
                avm2::Instruction ins = avm2::decode(body.code.data(), body.code.size(), pc);
                if (ins.size == 0) break;
                if (ins.info && ins.info->nameOperand != avm2::kNoOperand &&
                    ins.info->nameOperand < ins.operands.size()) {
                    uint32_t mn = static_cast<uint32_t>(ins.operands[ins.info->nameOperand]);
                    std::string lbl = (mn < abc_.multinames.size())
                        ? abc_.str(abc_.multinames[mn].name) : "?";
                    if (lbl.find(needle) != std::string::npos)
                        std::fprintf(stderr, "[find] '%s' mi=%u pc=%zu op=%s\n",
                                     lbl.c_str(), body.methodIndex, pc,
                                     ins.info->valid() ? ins.info->name : "db");
                }
                pc += ins.size;
            }
        }
    }
    if (std::getenv("SWF_AS_DEBUG")) {
        for (size_t i = 0; i < 10 && i < abc_.methods.size(); ++i)
            std::fprintf(stderr, "[as] method[%zu] nameIdx=%u name='%s' params=%u body=%d\n", i,
                         abc_.methods[i].nameIndex, abc_.methods[i].name.c_str(),
                         abc_.methods[i].param_count, abc_.methods[i].bodyIndex);
        for (size_t i = 0; i < 5 && i < abc_.methodBodies.size(); ++i)
            std::fprintf(stderr, "[as] body[%zu] methodIndex=%u size=%zu\n", i,
                         abc_.methodBodies[i].methodIndex, abc_.methodBodies[i].code.size());
        // 全量具名放置（按 Sprite characterId 去重）：核对 getChildByName /
        // clip["instanceName"] 依赖的实例名是否真的存在于 SWF 显示列表。
        std::map<uint16_t, std::set<std::string>> namedByCid;
        for (const auto& kv : movie_.characters) {
            if (!kv.second || kv.second->kind != Character::Kind::Sprite) continue;
            for (const auto& fr : kv.second->spriteFrames)
                for (const auto& di : fr.items)
                    if (!di.name.empty()) namedByCid[kv.first].insert(di.name);
        }
        for (const auto& kv : namedByCid) {
            std::string s;
            for (const auto& n : kv.second) s += n + " ";
            std::string line = "[as] named cid=" + std::to_string(kv.first) + " [" + s + "]\n";
            std::fputs(line.c_str(), stderr);
        }
        // 关卡 stepMC 的前几帧具名清单：as3 的 frame1 脚本会访问
        // this["gensui"] 等跨帧实例，需确认它们在当前帧是否被放置。
        for (uint16_t cid : {750u, 932u, 1042u, 1108u, 1236u}) {
            auto it = movie_.characters.find(cid);
            if (it == movie_.characters.end() || !it->second ||
                it->second->kind != Character::Kind::Sprite) continue;
            const auto& sf = it->second->spriteFrames;
            for (size_t fi = 0; fi < sf.size() && fi < 3; ++fi) {
                std::string s;
                for (const auto& di : sf[fi].items)
                    if (!di.name.empty()) s += di.name + " ";
                std::string line = "[as] tl cid=" + std::to_string(cid) + " f" +
                    std::to_string(fi) + " named=[" + s + "]\n";
                std::fputs(line.c_str(), stderr);
            }
        }
        for (const auto& kv : movie_.symbolClasses)
            if (kv.first.find("ProgressBar") != std::string::npos)
                std::fprintf(stderr, "[as] symbolClass cid=%u cls=%s\n",
                             static_cast<unsigned>(kv.second), kv.first.c_str());
        // 关卡首帧放置明细：便于把具名实例映射到舞台坐标并复现拖拽。
        for (uint16_t cid : {750u, 932u, 1042u}) {
            auto it = movie_.characters.find(cid);
            if (it == movie_.characters.end() || !it->second ||
                it->second->kind != Character::Kind::Sprite ||
                it->second->spriteFrames.empty()) continue;
            for (const auto& di : it->second->spriteFrames[0].items)
                std::fprintf(stderr,
                    "[as] item cid=%u f0 name='%s' child=%u depth=%u x=%.0f y=%.0f\n",
                    static_cast<unsigned>(cid), di.name.c_str(),
                    static_cast<unsigned>(di.characterId),
                    static_cast<unsigned>(di.depth), di.matrix.tx / 20.0,
                    di.matrix.ty / 20.0);
        }
    }
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
    reportFvmUnknownOps("run");
}

// 推进一个显示对象及其子对象的时间轴（动画驱动）。
void Vm::advanceTimeline(const std::shared_ptr<DisplayObject>& d) {
    if (!d) return;
    // 任何指向 DefineSprite 且处于播放态的显示对象都按帧循环推进。
    // Flash 语义上"可播放的精灵"由"characterId 指向 DefineSprite"决定，
    // 与运行时包装对象的 ObjKind 无关；只认 MovieClip 会漏掉一部分
    // 带时间轴补间/帧脚本的精灵（它们的 currentFrame 停在 0，帧脚本不触发）。
    // 根时间轴（stage 根，characterId==0，parent==null）同样按 movie_.frames
    // 逐帧推进——这是根第 1 帧内容得以显示与等待点击的前提；
    // stop()/play() 原生已作用于根的 playing 标志。
    const bool isStageRoot = !d->parent;
    if (std::getenv("SWF_AS_DEBUG") && (isStageRoot || d->characterId == 0))
        std::fprintf(stderr, "[as] adv cls=%s cid=%d parent=%s playing=%d frame=%d lastScript=%d\n",
                     d->name.c_str(), d->characterId, d->parent ? "Y" : "N",
                     (int)d->playing, d->currentFrame, d->lastScriptFrame);
    if (d->playing && (d->characterId || isStageRoot)) {
        const std::vector<Frame>* frames = nullptr;
        if (d->characterId) {
            auto it = movie_.characters.find(d->characterId);
            if (it != movie_.characters.end() && it->second &&
                it->second->kind == Character::Kind::Sprite)
                frames = &it->second->spriteFrames;
        } else if (isStageRoot) {
            frames = &movie_.frames;
        }
        if (frames) {
            int total = static_cast<int>(frames->size());
            if (total > 0) {
                // 新实例先停留在首帧：让 addFrameScript(0, ...) 有机会执行
                // （否则创建当帧就被推进到第 2 帧，首帧脚本永远不触发）。
                // FIX-2026-017：仅对注册了第 0 帧脚本的对象保持首帧。此前所有
                // 新建精灵一律停首帧一拍，而 lastScriptFrame 只有执行过帧脚本
                // 才 >=0，导致**无任何帧脚本的纯补间动画精灵**（转场 loading 屏的
                // 泡泡/白云/水瓶 cid770/767/552 等）lastScriptFrame 永远为 -1，
                // 被永久锁死在第 0 帧，动画整体冻结。
                // FIX-2026-024（已回退）：曾试过默认关闭持有以让加载转盘转动，
                // 但这会**跳过所有第 0 帧脚本**——大量本应停在首帧的剪辑（其
                // addFrameScript(0,..) 调用 stop()）全部失控播放（用户反馈
                // "很多都没有 stop"）。首帧持有是正确的 AS3 语义，必须保留。
                // 加载转盘的旋转应走脚本驱动路径（enterFrame/_rotation），
                // 见 FIX-2026-025。
                // 根时间轴（cid==0）同样必须首帧持有：主时间轴第 1 帧上的子剪辑
                // （如 preloader）注册的 addFrameScript(0,..) 中含 stop()/parent.stop()，
                // 若根在第一次 tick 就推进到第 2 帧，这些首帧脚本永远不执行、首页被跳过。
                // 持有只持续一拍：runFrameScripts 随后把 lastScriptFrame 置 0，下一 tick
                // 条件失效；若脚本调用了 parent.stop()，根的 playing 已为 false 不再推进。
                const bool holdFirst =
                    (isStageRoot && d->currentFrame == 0 && d->lastScriptFrame < 0) ||
                    (d->currentFrame == 0 && d->lastScriptFrame < 0 &&
                     d->characterId && d->frameScripts.count(0));
                if (holdFirst) {
                    // 保持第 0 帧，脚本由本帧稍后的 runFrameScripts 执行
                } else {
                    ++d->currentFrame;
                    if (d->currentFrame >= total) d->currentFrame = 0;   // 循环播放
                    applyTimelineFrame(d, d->currentFrame);              // 进入新帧：重建放置列表
                }
            }
        }
    }
    for (auto& c : d->children) advanceTimeline(c);
}

// 进入新帧时执行该显示对象注册的 frame script（addFrameScript）。
void Vm::runFrameScripts(const std::shared_ptr<DisplayObject>& d) {
    if (!d) return;
    if (d->currentFrame != d->lastScriptFrame) {
        d->lastScriptFrame = d->currentFrame;
        auto it = d->frameScripts.find(d->currentFrame);
        if (it != d->frameScripts.end() && it->second.kind == VKind::Object && it->second.obj) {
            if (std::getenv("SWF_AS_DEBUG")) {
                std::string ownerCls = "-";
                if (auto ow = d->owner.lock()) ownerCls = ow->className;
                std::fprintf(stderr, "[as] runFrameScript frame=%d obj=%s owner=%s dispName=%s\n",
                             d->currentFrame, it->second.obj->className.c_str(),
                             ownerCls.c_str(), d->name.c_str());
            }
            const char* flt = std::getenv("SWF_AS_OPS");
            const bool mine = flt && it->second.obj->className.find(flt) != std::string::npos;
            if (mine) g_traceOpsActive = true;
            invokeValue(it->second, {}, d->owner.lock());
            if (mine) g_traceOpsActive = false;
        }
    }
    for (auto& c : d->children) runFrameScripts(c);
}

// 向所有注册了该类型监听器的对象派发事件（以注册者自身作为回调 this）。
void Vm::dispatchToAll(const std::string& type) {
    auto ev = std::make_shared<Object>();
    ev->kind = ObjKind::Event;
    ev->className = "Event";
    ev->props["type"] = Value::str(type);

    // 快照对象表，避免回调中新增/删除对象导致迭代器失效
    std::vector<std::shared_ptr<Object>> snap = objects_;
    const bool dbg = std::getenv("SWF_AS_DEBUG") != nullptr;
    if (dbg) {
        std::size_t nl = 0;
        for (auto& o : snap) if (o && o->listeners.count(type)) ++nl;
        std::fprintf(stderr, "[as] dispatch %s objects=%zu listeners=%zu", type.c_str(), snap.size(), nl);
        for (auto& o : snap)
            if (o && o->listeners.count(type)) {
                std::fprintf(stderr, " [%s fn:", o->className.c_str());
                for (auto& lv : o->listeners[type])
                    std::fprintf(stderr, " %s", (lv.obj ? lv.obj->className.c_str() : "-"));
                std::fprintf(stderr, "]");
            }
        std::fprintf(stderr, "\n");
    }
    for (auto& obj : snap) {
        if (!obj) continue;
        auto lit = obj->listeners.find(type);
        if (lit == obj->listeners.end()) continue;
        std::vector<Value> ls = lit->second;          // 监听器快照
        // Flash 语义：监听者自身即事件目标（onE 等用 e.currentTarget.mouseX 取坐标）
        ev->props["target"] = Value::object(obj);
        ev->props["currentTarget"] = Value::object(obj);
        for (auto& lv : ls) {
            // SWF_AS_OPS 事件派发版钩子：监听闭包类名匹配时打开 opcode 轨迹
            //（帧脚本路径在 runFrameScripts 已有同类钩子；事件监听器如 progreso
            // 是普通闭包，方法名为空，只能按闭包类名匹配）。
            const char* flt = std::getenv("SWF_AS_OPS");
            const bool mine = flt && lv.obj && lv.obj->className.find(flt) != std::string::npos;
            if (mine) g_traceOpsActive = true;
            invokeValue(lv, {Value::object(ev)}, obj);
            if (mine) g_traceOpsActive = false;
        }
    }
}

// TweenLite/TweenMax 补间：按同一虚拟时钟插值目标属性；到达终点后触发
// onComplete。回调中可能再创建补间，所以先把完成项摘出再统一派发。
void Vm::advanceTweens() {
    if (tweens_.empty()) return;
    std::vector<Value> callbacks;
    std::vector<std::vector<Value>> callbackArgs;
    std::vector<TweenTask> pending;
    for (auto& t : tweens_) {
        const double elapsed = timerNowMs_ - t.startMs - t.delayMs;
        if (elapsed < 0.0) { pending.push_back(t); continue; }   // delay 未到
        const bool done = t.durationMs <= 0.0 || elapsed >= t.durationMs;
        const double k = done ? 1.0 : elapsed / t.durationMs;
        if (t.target) {
            for (const auto& kv : t.fromTo)
                setProperty(t.target, kv.first,
                            Value::num(kv.second.first +
                                       (kv.second.second - kv.second.first) * k));
        }
        if (done) {
            if (t.hasComplete) {
                callbacks.push_back(t.onComplete);
                callbackArgs.push_back(t.completeParams);
            }
        } else {
            pending.push_back(t);
        }
    }
    tweens_.swap(pending);
    for (std::size_t i = 0; i < callbacks.size(); ++i)
        invokeValue(callbacks[i], callbackArgs[i], nullptr);
}

// 播放一帧：先推进所有时间轴，再派发 Flash 标准帧事件。
// 本地整片加载语义：向已创建且已有监听器的 loaderInfo 派发 init/progress/complete。
// 监听器注册时机不定（构造函数/帧脚本均可），故惰性派发：每个 loaderInfo 在
// 首次"有监听器"的 tick 补发一次完整加载事件序列；事件携带 bytesLoaded/bytesTotal。
void Vm::fireLoaderEvents() {
    if (loaderInfos_.empty()) return;
    auto evC = globals_->props.find("ProgressEvent");
    if (evC == globals_->props.end() || evC->second.kind != VKind::Object || !evC->second.obj)
        evC = globals_->props.find("Event");
    if (evC == globals_->props.end() || evC->second.kind != VKind::Object || !evC->second.obj)
        return;
    auto mkEv = [&](const char* type) -> Value {
        Value ev = constructObject(evC->second, {Value::str(type)});
        if (ev.kind == VKind::Object && ev.obj) {
            ev.obj->props["bytesLoaded"] = Value::intg(100);
            ev.obj->props["bytesTotal"] = Value::intg(100);
        }
        return ev;
    };
    std::vector<std::shared_ptr<Object>> snap = loaderInfos_;
    for (auto& info : snap) {
        if (!info || info->listeners.empty()) continue;
        auto fired = info->props.find("$loaderFired");
        if (fired != info->props.end() && fired->second.toBool()) continue;
        info->props["$loaderFired"] = Value::boolean(true);
        if (std::getenv("SWF_AS_DEBUG"))
            std::fprintf(stderr, "[as] loader events -> %s (listeners=%zu)\n",
                         info->className.c_str(), info->listeners.size());
        callMethod(info, "dispatchEvent", {mkEv("init")});
        callMethod(info, "dispatchEvent", {mkEv("progress")});
        callMethod(info, "dispatchEvent", {mkEv("complete")});
    }
}

void Vm::tick() {
    const bool dbg = std::getenv("SWF_AS_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[as] tick begin stage=%zu objects=%zu\n", stage.size(), objects_.size());
    fireLoaderEvents();
    // 定时器：虚拟时间按舞台帧率推进；到期的回调在帧事件前执行（Flash 语义近似）。
    timerNowMs_ += 1000.0 / (movie_.frameRate > 0 ? movie_.frameRate : 12.0);
    std::vector<int> dueIds;
    for (auto& t : timers_)
        if (t.dueMs <= timerNowMs_) dueIds.push_back(t.id);
    for (int id : dueIds) {
        auto it = std::find_if(timers_.begin(), timers_.end(),
                               [id](const TimerTask& t) { return t.id == id; });
        if (it == timers_.end()) continue;      // 已被回调清掉
        TimerTask t = *it;                       // 拷贝：回调中可能增删 timers_
        if (t.intervalMs > 0) it->dueMs = timerNowMs_ + t.intervalMs;
        else timers_.erase(it);
        if (dbg) std::fprintf(stderr, "[as] timer fire id=%d\n", t.id);
        invokeValue(t.fn, t.args, nullptr);
    }
    // flash.utils.Timer：到期的 Timer 只向自身注册的监听器派发，
    // 不能走 dispatchToAll（那会唤醒所有对象上的同名监听器）。
    {
        std::vector<std::weak_ptr<Object>> snap;
        snap.reserve(timerObjects_.size());
        for (const auto& t : timerObjects_) snap.push_back(t.obj);
        auto fire = [this](const std::shared_ptr<Object>& to, const char* type) {
            auto ev = newObject(ObjKind::Event, "TimerEvent");
            ev->props["type"] = Value::str(type);
            ev->props["target"] = Value::object(to);
            ev->props["currentTarget"] = Value::object(to);
            auto lit = to->listeners.find(type);
            if (lit == to->listeners.end()) return;
            const std::vector<Value> ls = lit->second;
            for (const auto& lv : ls) invokeValue(lv, {Value::object(ev)}, to);
        };
        for (const auto& wp : snap) {
            auto to = wp.lock();
            if (!to) continue;
            TimerObjectTask* t = nullptr;
            for (auto& q : timerObjects_) {
                auto o = q.obj.lock();
                if (o && o.get() == to.get()) { t = &q; break; }
            }
            if (!t || !t->running || t->dueMs > timerNowMs_) continue;
            ++t->currentCount;
            const int cnt = t->currentCount;
            const int rep = t->repeatCount;
            const double delay = t->delayMs > 0 ? t->delayMs : 1.0;
            const bool complete = rep > 0 && cnt >= rep;
            if (complete) t->running = false;
            else {
                t->dueMs += delay;
                if (t->dueMs <= timerNowMs_) t->dueMs = timerNowMs_ + delay;
            }
            fire(to, "timer");          // 回调可能增删 timerObjects_，此后不再使用 t
            if (complete) fire(to, "timerComplete");
        }
    }
    for (auto& d : stage) advanceTimeline(d);
    for (auto& d : stage) runFrameScripts(d);
    // 拖动位置的应用时机必须是时间轴更新之后：applyTimelineFrame 每帧会把子实例的
    // x/y 重置回时间轴矩阵值，若拖动位置只在鼠标事件时更新，播放中的被拖对象会
    // 逐帧在“时间轴位置”和“鼠标位置”之间来回跳（表现为乱晃）。
    updateDragPosition();
    // 补间属性必须在 enterFrame 监听器之前就位（Flash 的 TweenLite 也基于帧事件）。
    advanceTweens();
    if (dbg) std::fprintf(stderr, "[as] advanceTimeline done\n");
    dispatchToAll("enterFrame");
    if (dbg) std::fprintf(stderr, "[as] enterFrame done\n");
    dispatchToAll("frameConstructed");
    if (dbg) std::fprintf(stderr, "[as] frameConstructed done\n");
    dispatchToAll("exitFrame");
    if (dbg) std::fprintf(stderr, "[as] exitFrame done\n");
    reportFvmUnknownOps("tick");
}

// ---------- 鼠标输入与事件派发 ----------
// 显示对象局部 twips -> 舞台像素的世界变换（与 Render.cpp 的 renderDisplayObject 一致）。
static Matrix displayWorldMatrix(const std::shared_ptr<DisplayObject>& d, const Movie& movie) {
    std::vector<const DisplayObject*> chain;
    for (auto c = d; c; c = c->parent) chain.push_back(c.get());
    Matrix world;
    world.sx = 1.0f / 20.0f; world.sy = 1.0f / 20.0f;
    world.tx = -movie.frameSize.x0 / 20.0f;
    world.ty = -movie.frameSize.y0 / 20.0f;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const DisplayObject& o = **it;
        Matrix lt;
        if (o.hasFull) {
            lt.sx = static_cast<float>(o.fullA);
            lt.r0 = static_cast<float>(o.fullB);
            lt.r1 = static_cast<float>(o.fullC);
            lt.sy = static_cast<float>(o.fullD);
        } else {
            double rad = o.rotation * 3.141592653589793 / 180.0;
            double ca = std::cos(rad), sa = std::sin(rad);
            lt.sx = static_cast<float>(o.scaleX * ca);
            lt.r0 = static_cast<float>(-o.scaleX * sa);
            lt.r1 = static_cast<float>(o.scaleY * sa);
            lt.sy = static_cast<float>(o.scaleY * ca);
        }
        lt.tx = static_cast<float>(o.x * 20.0);
        lt.ty = static_cast<float>(o.y * 20.0);
        world = world.post(lt);
    }
    return world;
}

// 舞台像素点 -> 对象局部像素坐标（AS3 mouseX/mouseY、MouseEvent.localX 语义）。
static bool displayLocalPoint(const std::shared_ptr<DisplayObject>& d, const Movie& movie,
                              double sx, double sy, double& lx, double& ly) {
    if (!d) return false;
    Matrix inv;
    if (!displayWorldMatrix(d, movie).invert(inv)) return false;
    float ox = 0, oy = 0;
    inv.apply(static_cast<float>(sx), static_cast<float>(sy), ox, oy);
    lx = ox / 20.0;   // 局部 twips -> 局部像素
    ly = oy / 20.0;
    return true;
}

// 显示对象对应的 AS3 对象：优先时间轴/代码创建的实例；根为 Stage；其余动态包装。
std::shared_ptr<Object> Vm::objectForDisplay(const std::shared_ptr<DisplayObject>& d) {
    if (!d) return nullptr;
    if (auto ow = d->owner.lock()) return ow;
    if (!d->parent) {
        auto& cache = stageCache_[d.get()];
        if (!cache) {
            cache = newObject(ObjKind::Stage, "Stage");
            cache->display = d;
            d->owner = cache;
            auto g = globals_->props.find("Stage");
            if (g != globals_->props.end() && g->second.kind == VKind::Object && g->second.obj)
                cache->cls = g->second.obj->classDef;
        }
        return cache;
    }
    auto o = newObject(d->kind == ObjKind::DisplayObject ? ObjKind::MovieClip : d->kind, "DisplayObject");
    o->display = d;
    d->owner = o;
    return o;
}

// 临时诊断（SWF_AS_DUMP_TREE）：打印运行时显示树（含舞台世界坐标，便于定位点击点）。
static void dumpDisplayTree(const std::shared_ptr<DisplayObject>& d, int level, const Movie& movie) {
    if (!d) return;
    Matrix w = displayWorldMatrix(d, movie);   // 已是设备像素
    std::string ownerCls = "-";
    if (auto ow = d->owner.lock()) ownerCls = ow->className;
    std::printf("[tree] lvl=%d name=%s cid=%u depth=%d kids=%zu f=%d built=%d vis=%d "
                "xy=(%.1f,%.1f) world=(%.1f,%.1f) scale=(%.2f,%.2f) rot=%.1f cls=%s\n",
                level, d->name.c_str(), static_cast<unsigned>(d->characterId), d->depth,
                d->children.size(), d->currentFrame, static_cast<int>(d->timelineBuilt),
                static_cast<int>(d->visible),
                d->x, d->y, w.tx, w.ty, d->scaleX, d->scaleY, d->rotation, ownerCls.c_str());
    if (d->characterId) {
        auto it = movie.characters.find(d->characterId);
        if (it != movie.characters.end() && it->second && it->second->shape) {
            const RectB& b = it->second->shape->bounds;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            const float cxs[4] = {static_cast<float>(b.x0), static_cast<float>(b.x1),
                                  static_cast<float>(b.x1), static_cast<float>(b.x0)};
            const float cys[4] = {static_cast<float>(b.y0), static_cast<float>(b.y0),
                                  static_cast<float>(b.y1), static_cast<float>(b.y1)};
            for (int k = 0; k < 4; ++k) {
                float px = 0, py = 0;
                w.apply(cxs[k], cys[k], px, py);
                x0 = std::min(x0, px); y0 = std::min(y0, py);
                x1 = std::max(x1, px); y1 = std::max(y1, py);
            }
            std::printf("       bounds twips=(%d,%d)-(%d,%d) dev=(%.1f,%.1f)-(%.1f,%.1f)\n",
                        b.x0, b.y0, b.x1, b.y1, x0, y0, x1, y1);
        }
        // 临时诊断：按钮命中区域在设备像素下的包围盒（用于定位 --click 坐标）
        if (it->second && it->second->kind == Character::Kind::Button && it->second->button) {
            const auto& bd = *it->second->button;
            const auto& bucket = !bd.hit.empty() ? bd.hit : bd.up;
            for (const auto& sub : bucket) {
                if (!sub.character || !sub.character->shape) continue;
                const RectB& b = sub.character->shape->bounds;
                Matrix m = w.post(sub.matrix);
                float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                const float cxs[4] = {static_cast<float>(b.x0), static_cast<float>(b.x1),
                                      static_cast<float>(b.x1), static_cast<float>(b.x0)};
                const float cys[4] = {static_cast<float>(b.y0), static_cast<float>(b.y0),
                                      static_cast<float>(b.y1), static_cast<float>(b.y1)};
                for (int k = 0; k < 4; ++k) {
                    float px = 0, py = 0; m.apply(cxs[k], cys[k], px, py);
                    bx0 = std::min(bx0, px); by0 = std::min(by0, py);
                    bx1 = std::max(bx1, px); by1 = std::max(by1, py);
                }
                std::printf("       btnHit shape=%u dev=(%.1f,%.1f)-(%.1f,%.1f)\n",
                            static_cast<unsigned>(sub.characterId), bx0, by0, bx1, by1);
            }
        }
    }
    if (level < 8)
        for (auto& c : d->children) dumpDisplayTree(c, level + 1, movie);
}

std::vector<std::shared_ptr<DisplayObject>> Vm::hitChain(double x, double y) {
    if (std::getenv("SWF_AS_DUMP_TREE")) {
        const char* msEnv = std::getenv("SWF_AS_DUMP_MS");   // 仅在该虚拟时刻之后打印（临时诊断）
        if (!msEnv || timerNowMs_ >= std::atof(msEnv))
            for (auto& r : stage) dumpDisplayTree(r, 0, movie_);
    }
    std::vector<std::shared_ptr<DisplayObject>> chain;
    if (!hitTestRuntimeStage(movie_, stage, x, y, &chain) || chain.empty()) {
        chain.clear();
        if (!stage.empty()) chain.push_back(stage.front());   // 空白区域仍由舞台接收
    }
    return chain;
}

void Vm::dispatchMouseEvent(const std::string& type,
                            const std::vector<std::shared_ptr<DisplayObject>>& chain,
                            bool bubbles) {
    auto ev = std::make_shared<Object>();
    ev->kind = ObjKind::Event;
    ev->className = "MouseEvent";
    ev->props["type"] = Value::str(type);
    ev->props["bubbles"] = Value::boolean(bubbles);
    ev->props["cancelable"] = Value::boolean(false);
    ev->props["stageX"] = Value::num(mouseX_);
    ev->props["stageY"] = Value::num(mouseY_);
    double lx = mouseX_, ly = mouseY_;
    if (!chain.empty()) {
        displayLocalPoint(chain.back(), movie_, mouseX_, mouseY_, lx, ly);
        ev->props["target"] = Value::object(objectForDisplay(chain.back()));
    }
    ev->props["localX"] = Value::num(lx);
    ev->props["localY"] = Value::num(ly);

    // 事件目标（链尾）-> 祖先（链首）-> Stage（显示树最外层祖先）
    std::vector<std::shared_ptr<Object>> targets;
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
        auto o = objectForDisplay(chain[static_cast<std::size_t>(i)]);
        if (o) targets.push_back(o);
    }
    if (!chain.empty()) {
        auto top = chain.front();
        while (top && top->parent) top = top->parent;
        if (top) {
            auto& st = stageCache_[top.get()];
            if (!st) {
                st = newObject(ObjKind::Stage, "Stage");
                st->display = top;
                // 只在根对象尚无 AS3 包装时接管 owner。文档类实例（MovieClip 子类）
                // 才是 Flash 的 root，事件派发若无条件覆盖会让它退化为没有
                // play/gotoAndStop 的 Stage 包装，门户 Play 按钮因此无法推进主时间轴。
                if (!top->owner.lock()) top->owner = st;
                auto g = globals_->props.find("Stage");
                if (g != globals_->props.end() && g->second.kind == VKind::Object && g->second.obj)
                    st->cls = g->second.obj->classDef;
            }
            if (std::find(targets.begin(), targets.end(), st) == targets.end()) targets.push_back(st);
        }
    }

    if (std::getenv("SWF_AS_DEBUG")) {
        std::size_t nl = 0;
        for (auto& o : targets) if (o->listeners.count(type)) ++nl;
        std::fprintf(stderr, "[mouse] %s (%.0f,%.0f) target=%s depth=%d chain=%zu listeners=%zu\n",
                     type.c_str(), mouseX_, mouseY_,
                     chain.empty() ? "-" : chain.back()->name.c_str(),
                     chain.empty() ? -1 : chain.back()->depth, targets.size(), nl);
        for (auto& d : chain)
            std::fprintf(stderr, "   chain name=%s cid=%u depth=%d\n", d->name.c_str(),
                         static_cast<unsigned>(d->characterId), d->depth);
        for (auto& o : targets) {
            std::fprintf(stderr, "   target %s keys=", o->className.c_str());
            for (auto& kv : o->listeners)
                std::fprintf(stderr, " %s(%zu)", kv.first.c_str(), kv.second.size());
            std::fprintf(stderr, "\n");
        }
    }

    // bubbles=false 时只派发目标；同时尊重脚本设置的 stopPropagation。
    auto stopped = [&](const char* n) {
        const auto p = ev->props.find(n);
        return p != ev->props.end() && p->second.toBool();
    };
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (stopped("$stopImmediate")) break;
        ev->props["currentTarget"] = Value::object(targets[i]);
        ev->props["eventPhase"] = Value::intg(i == 0 ? 2 : 3);
        const auto lit = targets[i]->listeners.find(type);
        if (lit != targets[i]->listeners.end()) {
            const std::vector<Value> ls = lit->second;   // 回调中可能增删监听
            for (const auto& lv : ls) {
                if (stopped("$stopImmediate")) break;
                invokeValue(lv, {Value::object(ev)}, targets[i]);
            }
        }
        if (!bubbles || stopped("$stop")) break;
    }
    ev->props["currentTarget"] = Value::nullv();
}

// 拖动中的对象跟随指针：把舞台像素坐标换算到父级局部像素后更新 x/y。
void Vm::updateDragPosition() {
    auto d = dragTarget_;
    if (!d || !d->parent) return;
    double lx = 0, ly = 0;
    if (!displayLocalPoint(d->parent, movie_, mouseX_, mouseY_, lx, ly)) return;
    d->x = lx - dragOffsetX_;
    d->y = ly - dragOffsetY_;
}

void Vm::setMousePosition(double x, double y) {
    mouseX_ = x;
    mouseY_ = y;
    updateDragPosition();
    auto chain = hitChain(x, y);
    if (chain != mouseOverChain_) {
        // 离开旧链上的按钮：恢复 Up 态
        for (auto& d : mouseOverChain_)
            if (isButtonDisplay(movie_, d) &&
                std::find(chain.begin(), chain.end(), d) == chain.end())
                d->buttonState = 0;
        if (!mouseOverChain_.empty()) dispatchMouseEvent("mouseOut", mouseOverChain_, true);
        dispatchMouseEvent("mouseOver", chain, true);
        mouseOverChain_ = chain;
    }
    // 命中链上的按钮：指针在按钮上为 Over，按下且仍在被按按钮内为 Down
    for (auto& d : chain) {
        if (!isButtonDisplay(movie_, d)) continue;
        d->buttonState = (mouseDown_ && mouseDownTarget_ && containsDisplay(d, mouseDownTarget_)) ? 2 : 1;
    }
    dispatchMouseEvent("mouseMove", chain, true);
}

void Vm::mouseDown(double x, double y) {
    setMousePosition(x, y);
    auto chain = hitChain(x, y);
    mouseDown_ = true;
    mouseDownTarget_ = chain.empty() ? nullptr : chain.back();
    for (auto& d : chain)
        if (isButtonDisplay(movie_, d) && containsDisplay(d, mouseDownTarget_))
            d->buttonState = 2;
    dispatchMouseEvent("mouseDown", chain, true);
}

void Vm::mouseUp(double x, double y) {
    setMousePosition(x, y);
    auto chain = hitChain(x, y);
    dispatchMouseEvent("mouseUp", chain, true);
    // Flash 语义：startDrag 是舞台级持续状态，松开鼠标不会自动结束；
    // 只有脚本调用 stopDrag()（或另一个 startDrag 接管）才停止跟随。
    if (mouseDown_) {
        auto isAncestor = [](const std::shared_ptr<DisplayObject>& a,
                             const std::shared_ptr<DisplayObject>& b) {
            for (auto c = b; c; c = c->parent)
                if (c == a) return true;
            return false;
        };
        auto up = chain.empty() ? nullptr : chain.back();
        if (mouseDownTarget_ && up &&
            (mouseDownTarget_ == up || isAncestor(mouseDownTarget_, up) ||
             isAncestor(up, mouseDownTarget_)))
            dispatchMouseEvent("click", chain, true);
    }
    mouseDown_ = false;
    mouseDownTarget_.reset();
    // 抬起后指针仍在按钮上：回到 Over 态
    for (auto& d : chain)
        if (isButtonDisplay(movie_, d)) d->buttonState = 1;
}

void Vm::mouseClick(double x, double y) {
    mouseDown(x, y);
    mouseUp(x, y);
}

// ---- 解释器 ----
namespace {
// AVM2 递归调用此前没有全局深度上限（原先每层 exec 各有独立的 20 万步 guard），
// 用 thread_local 计数 + RAII 递减覆盖 exec 的所有 return 分支。
thread_local int g_execDepth = 0;
struct ExecDepthGuard {
    ExecDepthGuard() { ++g_execDepth; }
    ~ExecDepthGuard() { --g_execDepth; }
};
constexpr int kMaxExecDepth = 64;
}  // namespace

Value Vm::exec(const abc::MethodBody& body, const std::vector<Value>& args,
               std::shared_ptr<Object> thisObj,
               const std::vector<std::shared_ptr<Object>>* capturedScope) {
    ExecDepthGuard depthGuard;
    if (g_execDepth > kMaxExecDepth) return Value::undefined();
    std::vector<Value> stack;
    stack.reserve(body.maxStack + 8);
    std::vector<Value> locals(body.localCount + 8);
    if (!locals.empty()) {
        locals[0] = thisObj ? Value::object(thisObj) : Value::undefined();
    }
    for (size_t i = 0; i < args.size() && i + 1 < locals.size(); ++i) locals[i + 1] = args[i];
    // AVM2 可选参数默认值：实参不足时，用 method_info 的 option_detail 填充末尾参数。
    if (body.methodIndex < abc_.methods.size()) {
        const auto& mi = abc_.methods[body.methodIndex];
        if (!mi.optionalDefaults.empty()) {
            size_t firstOpt = mi.param_count > mi.optionalDefaults.size()
                                  ? mi.param_count - mi.optionalDefaults.size() : 0;
            for (size_t j = 0; j < mi.optionalDefaults.size(); ++j) {
                size_t paramIdx = firstOpt + j;
                size_t localIdx = paramIdx + 1;
                if (args.size() > paramIdx || localIdx >= locals.size()) continue;
                const auto& d = mi.optionalDefaults[j];
                // AVM2 ConstantKind：0x00 Undefined / 0x01 Utf8 / 0x03 Int /
                // 0x04 UInt / 0x06 Double / 0x0A False / 0x0B True / 0x0C Null。
                // val 是对应常量池索引（False/True/Null/Undefined 时忽略）。
                Value dv = Value::undefined();
                switch (d.kind) {
                    case 0x00: dv = Value::undefined(); break;
                    case 0x01: dv = Value::str(abc_.str(d.val)); break;
                    case 0x03: dv = Value::intg(d.val < abc_.ints.size()
                                    ? static_cast<int32_t>(abc_.ints[d.val]) : 0); break;
                    case 0x04: dv = Value::uintv(d.val < abc_.uints.size()
                                    ? abc_.uints[d.val] : 0u); break;
                    case 0x06: dv = Value::num(d.val < abc_.doubles.size()
                                    ? abc_.doubles[d.val] : 0.0); break;
                    case 0x0A: dv = Value::boolean(false); break;
                    case 0x0B: dv = Value::boolean(true);  break;
                    case 0x0C: dv = Value::nullv();        break;
                    default: break;
                }
                locals[localIdx] = dv;
                if (std::getenv("SWF_AS_DEBUG"))
                    std::fprintf(stderr, "[as] defaultparam mi=%u local=%zu kind=%u val=%u -> %s\n",
                                 body.methodIndex, localIdx, d.kind, d.val, dv.toString().c_str());
            }
        }
    }
    std::vector<std::shared_ptr<Object>> scope;
    if (capturedScope) scope = *capturedScope;
    const size_t baseScopeSize = scope.size();   // catch 入口需要把 try 内 push 的 scope 退掉
    // 注：方法体自身会通过 getlocal0/pushscope 建立作用域（含 activation），此处不再隐式加入 this

    Inst in(body.code);
    // 临时诊断：SWF_AS_TRACE=1 打印方法体执行概况与未捕获异常
    const bool traceExec = std::getenv("SWF_AS_TRACE") != nullptr;
    int traceBody = -1;
    if (!abc_.methodBodies.empty()) {
        auto* base = &abc_.methodBodies[0];
        if (&body >= base && &body < base + abc_.methodBodies.size())
            traceBody = static_cast<int>(&body - base);
    }
    if (traceExec && !abc_.methodBodies.empty()) {
        const char* methodName = "-";
        if (body.methodIndex < abc_.methods.size())
            methodName = abc_.methods[body.methodIndex].name.c_str();
        std::fprintf(stderr, "[as] exec body=%d method=%s size=%zu maxStack=%u locals=%u this=%s traits=%zu\n",
                     traceBody, methodName, body.code.size(), body.maxStack, body.localCount,
                     thisObj ? thisObj->className.c_str() : "-", body.traits.size());
        if (traceBody == 54)
            for (const auto& t : body.traits)
                std::fprintf(stderr, "   activation trait %s kind=%d slot=%u\n",
                             multinameName(t.name).c_str(), t.kind, t.slotId);
    }
    auto finish = [&](const char* how) -> Value {
        if (traceExec) std::fprintf(stderr, "[as] leave body=%d %s pc=%zu\n", traceBody, how, in.pos());
        return Value::undefined();
    };
    static const bool fvmInited = (registerFvmOps(), true);
    // SWF_AS_OPS=<子串>：打印匹配方法体实际执行的 opcode 序列（含 pc 与栈深），
    // 用于确认关键帧脚本/构造函数是否真的执行到目标逻辑。
    const char* opsFilter = std::getenv("SWF_AS_OPS");
    std::string opsName;
    bool traceOps = false;
    if (opsFilter) {
        if (body.methodIndex < abc_.methods.size()) opsName = abc_.methods[body.methodIndex].name;
        traceOps = opsName.find(opsFilter) != std::string::npos;
        // 类名匹配：SWF_AS_OPS=Service 跟踪该类实例上的所有方法体（游戏类方法体名
        // 常为空或与 trait 名不一致，按方法名过滤会漏）。
        if (!traceOps && thisObj) {
            std::string cn = thisObj->className;
            if (thisObj->cls && !thisObj->cls->name.empty()) cn += " " + thisObj->cls->name;
            if (cn.find(opsFilter) != std::string::npos) {
                traceOps = true;
                if (opsName.empty()) opsName = "<" + thisObj->className + ">";
            }
        }
    }
    if (g_traceOpsActive) traceOps = true;
    // SWF_AS_MI=<methodIndex>：按方法索引强制追踪（用于 getter 等匿名方法体）。
    if (const char* miEnv = std::getenv("SWF_AS_MI")) {
        if (body.methodIndex == static_cast<uint32_t>(std::atoi(miEnv))) { traceOps = true; if (opsName.empty()) opsName = "<mi" + std::string(miEnv) + ">"; }
    }
    size_t guard = 0;
    while (!in.atEnd()) {
        if (++guard > 200000) break;
        const size_t opPc = in.pos();
        uint8_t op = in.u8();
        if (traceOps) {
            // 附带操作数标签：调用/属性类 op 的 multiname 名、pushstring 的字符串，
            // 让帧脚本（闭包方法名为空）也能被人工读懂。
            std::string extra;
            auto peekU30 = [&body](size_t p, uint32_t& out) -> bool {
                uint32_t v = 0; int shift = 0;
                for (int i = 0; i < 5; ++i) {
                    if (p + static_cast<size_t>(i) >= body.code.size()) return false;
                    uint8_t b = body.code[p + static_cast<size_t>(i)];
                    v |= static_cast<uint32_t>(b & 0x7F) << shift;
                    if (!(b & 0x80)) { out = v; return true; }
                    shift += 7;
                }
                out = v;
                return true;
            };
            static const uint8_t kMnOps[] = {0x46, 0x4F, 0x4A, 0x5D, 0x5E, 0x60, 0x66, 0x61, 0x68, 0x80};
            for (uint8_t mo : kMnOps) {
                if (op == mo) {
                    uint32_t idx = 0;
                    if (peekU30(in.pos(), idx) && idx < abc_.multinames.size()) {
                        uint32_t ni = abc_.multinames[idx].name;
                        if (ni > 0 && ni < abc_.strings.size() && !abc_.strings[ni].empty())
                            extra += " [" + abc_.strings[ni] + "]";
                    }
                    break;
                }
            }
            if (op == 0x2C) {
                uint32_t idx = 0;
                if (peekU30(in.pos(), idx) && idx < abc_.strings.size())
                    extra += " '" + abc_.strings[idx] + "'";
            }
            if (op == 0x24) {  // pushbyte：紧跟 1 字节操作数
                size_t pp = in.pos();
                if (pp < body.code.size()) extra += " =" + std::to_string((int)body.code[pp]);
            }
            std::fprintf(stderr, "[ops] %s pc=%zu op=0x%02X sd=%zu%s\n",
                         opsName.c_str(), opPc, op, stack.size(), extra.c_str());
        }
        // M6：注册表统一分发（所有 AVM2 opcode 均已 command 化）
        if (auto* h = fvmOps().find(op)) {
            ExecCtx ec{stack, locals, scope, in, body, thisObj, *this};
            ec.codeSize = body.code.size();
            try {
                (*h)(ec);
            } catch (const AsThrow& t) {
                const int hIdx = ec.findHandler(opPc, t.value);
                if (hIdx >= 0) {
                    const auto& eh = body.exceptionHandlers;
                    const uint32_t target  = eh[static_cast<std::size_t>(hIdx) + 2];
                    const uint32_t varIdx  = eh[static_cast<std::size_t>(hIdx) + 4];
                    // ABC catch 入口：操作数栈清空后只压异常值，作用域栈退回到方法入口。
                    stack.clear();
                    stack.push_back(t.value);
                    if (scope.size() > baseScopeSize) scope.resize(baseScopeSize);
                    if (varIdx < locals.size()) locals[varIdx] = t.value;
                    if (target > body.code.size()) return Value::undefined();
                    in.seek(target);
                } else if (g_execDepth <= 1) {
                    // 顶层脚本未被捕获：与 Flash 一致地中止该脚本，不终止播放器。
                    if (std::getenv("SWF_AS_DEBUG") || std::getenv("SWF_AS_MISS"))
                        std::fprintf(stderr, "[as] uncaught exception: %s\n",
                                     t.value.toString().c_str());
                    return Value::undefined();
                } else {
                    throw;   // 继续向调用方（外层 exec）传播
                }
            }
            if (ec.done) return ec.returnValue;
            continue;
        }
        // 未注册 opcode：操作数紧跟其后，直接 continue 会把操作数当成下一条指令。
        // 现在用 AVM2 元数据表按规范长度跳过操作数，并按规范栈效应补/弹占位值，
        // 使方法体可以继续往下跑（对暂未实现的扩展指令做优雅降级），而不是整体错位。
        {
            const size_t pc = opPc;
            const swf::avm2::Instruction ins =
                swf::avm2::decode(body.code.data(), body.code.size(), pc);
            if (ins.size == 0) break;
            in.seek(pc + ins.size);
            const swf::avm2::OpInfo& oi = *ins.info;
            int namePops = 0;
            if (oi.nameOperand != swf::avm2::kNoOperand &&
                oi.nameOperand < ins.operands.size())
                namePops = mnRuntimeStackPops(abc_,
                    static_cast<uint32_t>(ins.operands[oi.nameOperand]));
            int pops = 0, pushes = 0;
            swf::avm2::stackEffect(ins, namePops, pops, pushes);
            const int delta = pushes - pops;
            if (delta < 0) {
                for (int k = 0; k < -delta && !stack.empty(); ++k) stack.pop_back();
            } else {
                for (int k = 0; k < delta; ++k) stack.push_back(Value::undefined());
            }

            const long n = ++g_unknownOps[op];
            if (std::getenv("SWF_AS_DEBUG") && n <= 2) {
                std::string hex;
                char b[8];
                for (size_t k = pc; k < body.code.size() && k < pc + 12; ++k) {
                    std::snprintf(b, sizeof(b), "%02X ", body.code[k]);
                    hex += b;
                }
                std::fprintf(stderr,
                             "[as] UNKNOWN op=0x%02X name=%s body=%d method=%s pc=%zu bytes=%s stack=%+d\n",
                             op, oi.name ? oi.name : "reserved", traceBody,
                             (body.methodIndex < abc_.methods.size()
                                  ? abc_.methods[body.methodIndex].name.c_str() : "-"),
                             pc, hex.c_str(), delta);
            }
            // SWF_AS_STRICT=1：未知指令立即终止方法体（便于定位解释器缺口）。
            if (std::getenv("SWF_AS_STRICT")) break;
        }
    }
    return finish("falloff");
}

Value Vm::execScript(const abc::MethodBody& body, std::shared_ptr<Object> thisObj) {
    return exec(body, {}, thisObj);
}

std::vector<std::shared_ptr<Vm>> runMovieScripts(Movie& movie, int initialFrame) {
    std::vector<std::shared_ptr<Vm>> vms;
    if (movie.abcFiles.empty()) return vms;
    for (const auto& abc : movie.abcFiles) {
        auto vm = std::make_shared<Vm>(abc, movie);
        vm->setInitialFrame(initialFrame);
        vm->run();
        if (!vm->stage.empty()) movie.runtimeStage = vm->stage;
        vms.push_back(vm);
    }
    return vms;
}

// ---------- opcode 覆盖度 ----------
// registerFvmOps() 只是把 handler 写入 map，重复调用是幂等的（覆盖同键）。
std::vector<std::uint8_t> implementedOpcodes() {
    registerFvmOps();
    std::vector<std::uint8_t> out;
    for (int code : fvmOps().codes())
        if (code >= 0 && code <= 0xFF) out.push_back(static_cast<std::uint8_t>(code));
    return out;
}

bool isOpcodeImplemented(std::uint8_t op) {
    registerFvmOps();
    return fvmOps().has(op);
}

}  // namespace fvm
}  // namespace swf
