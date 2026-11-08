// AVM1（ActionScript 1/2）动作字节流反汇编 + 最小解释器。
// 操作码编号严格对照 Adobe SWF 规范（ActionScript Bytecode）：
//   0x04 NextFrame .. 0x09 StopSounds；0x0A-0x18 算术/比较；0x1C/0x1D 变量；
//   0x3C DefineLocal；0x40 NewObject；0x4E/0x4F GetMember/SetMember；0x52/0x53 CallMethod/NewMethod；
//   0x96 Push；0x99 Jump；0x9D If；0x9F GotoFrame2；0x81 GotoFrame（0-based）等。
#include "swf/Avm1.hpp"
#include "swf/ByteView.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <functional>

namespace swf {
namespace avm1 {

// ---------- Value ----------
bool Value::toBool() const {
    switch (type) {
        case T::Number: return num != 0 && !std::isnan(num);
        case T::Bool:   return b;
        case T::String: return !str.empty();
        case T::Object: return true;
        case T::Null:   return false;
        case T::Undefined: return false;
    }
    return false;
}
double Value::toNumber() const {
    switch (type) {
        case T::Number: return num;
        case T::Bool:   return b ? 1 : 0;
        case T::Null:   return 0;
        case T::String: return std::strtod(str.c_str(), nullptr);
        case T::Undefined: return std::nan("");
        case T::Object: return 0;
    }
    return 0;
}
std::string Value::toString() const {
    char buf[64];
    switch (type) {
        case T::Number: {
            double n = num;
            if (std::isnan(n)) return "NaN";
            if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
            std::snprintf(buf, sizeof(buf), "%.6g", n);
            return buf;
        }
        case T::Bool:   return b ? "true" : "false";
        case T::String: return str;
        case T::Null:   return "null";
        case T::Undefined: return "undefined";
        case T::Object: return "[object Object]";
    }
    return "";
}

static Value makeClip(ITimeline* t) {
    Value v; v.type = Value::T::Object; v.isClip = true; v.clip = t; return v;
}
static Value makeIClip(IClip* c) {
    Value v; v.type = Value::T::Object; v.isClip = true; v.iclip = c;
    v.clip = nullptr; return v;
}
static Value makeObj(const std::string& klass) {
    Value v; v.type = Value::T::Object;
    v.obj = std::make_shared<std::map<std::string, Value>>();
    v.klass = klass;
    return v;
}

// 名字 -> 属性编号（-1 = 非内建属性）
static int clipPropByName(const std::string& n) {
    if (n == "_x") return PROP_X;
    if (n == "_y") return PROP_Y;
    if (n == "_xscale" || n == "_xScale") return PROP_XSCALE;
    if (n == "_yscale" || n == "_yScale") return PROP_YSCALE;
    if (n == "_currentframe" || n == "_currentFrame") return PROP_CURRENTFRAME;
    if (n == "_totalframes" || n == "_totalFrames") return PROP_TOTALFRAMES;
    if (n == "_alpha") return PROP_ALPHA;
    if (n == "_visible") return PROP_VISIBLE;
    if (n == "_width") return PROP_WIDTH;
    if (n == "_height") return PROP_HEIGHT;
    if (n == "_rotation") return PROP_ROTATION;
    if (n == "_target") return PROP_TARGET;
    if (n == "_framesloaded" || n == "_framesLoaded") return PROP_FRAMESLOADED;
    if (n == "_name") return PROP_NAME;
    if (n == "_xmouse" || n == "_xMouse") return PROP_XMOUSE;
    return -1;
}

// IClip 数值/布尔/字符串属性统一读取
static bool iclipGetProp(IClip* c, int propId, Value& out) {
    double d;
    if (c->getNumberProp(propId, d)) { out = Value(d); return true; }
    bool b;
    if (c->getBoolProp(propId, b)) { out = Value(b); return true; }
    std::string s;
    if (c->getStringProp(propId, s)) { out = Value(s); return true; }
    return false;
}

// 剪辑作用域变量：ClipEvent / 剪辑帧脚本里的**无前缀**变量即"该实例自身的属性"。
// Player 在执行这类脚本前把 currentThis 指向该剪辑的 IClip，读写都落到其 members
//（对 DisplayItem 的持久化 clipVars）。主时间轴脚本 currentThis 为空，仍走 globals。
static bool clipVarGet(const Machine& m, const std::string& n, Value& out) {
    if (!m.currentThis || !m.currentThis->members) return false;
    auto it = m.currentThis->members->find(n);
    if (it == m.currentThis->members->end()) return false;
    out = it->second;
    return true;
}
static void clipVarSet(Machine& m, const std::string& n, const Value& v) {
    if (m.currentThis && m.currentThis->members) (*m.currentThis->members)[n] = v;
}

// 带点路径的最后一段若不是显示项、而是剪辑上的属性/变量（如 "_root.T4.ob.price" 里的 price，
// 其中 T4/ob 是显示项、price 是 ob 的脚本变量），则整条路径解析不成 IClip。
// 这里退一步：把前缀解析为剪辑，再把最后一段按 属性→子剪辑→成员变量 的顺序取值。
// 返回 true 表示"已给出结果"（可能是 undefined）；false 表示前缀本身不是有效剪辑路径。
static bool resolveTrailingPath(ITimeline* timeline, const std::string& n, Value& out) {
    std::size_t pos = n.rfind('.');
    if (pos == std::string::npos || pos == 0) return false;
    std::string pre = n.substr(0, pos), last = n.substr(pos + 1);
    if (pre.empty() || last.empty()) return false;
    auto* sob = (IClip*)timeline->resolveClipPath(pre);
    if (!sob) return false;
    int pid = clipPropByName(last);
    if (pid >= 0 && iclipGetProp(sob, pid, out)) return true;
    if (void* ch = sob->findChildClip(last)) { out = makeIClip((IClip*)ch); return true; }
    auto it = sob->members->find(last);
    out = (it != sob->members->end()) ? it->second : Value::undef();
    return true;
}

// 小端读
static float readF32(const std::uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
static std::int32_t readI32(const std::uint8_t* p) {
    return (std::int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static std::int16_t readI16(const std::uint8_t* p) {
    return (std::int16_t)(p[0] | (p[1] << 8));
}
static std::uint16_t readU16(const std::uint8_t* p) {
    return (std::uint16_t)(p[0] | (p[1] << 8));
}

// ---------- 反汇编 ----------
Script disassemble(const std::uint8_t* data, std::size_t size) {
    Script s;
    if (!data || size == 0) return s;
    s.bytes.assign(data, data + size);
    ByteView bv(data, size);
    while (bv.pos() < size && !bv.overflow()) {
        int recStart = static_cast<int>(bv.pos());
        std::uint8_t code = bv.readUI8();
        int length = 0;
        int payloadStart = static_cast<int>(bv.pos());
        if (code & 0x80) {
            length = bv.readUI16();
            payloadStart = static_cast<int>(bv.pos());
        }
        // 说明：短动作（code<0x80）都没有内联 payload。CallMethod/NewMethod（0x52/0x53）
        // 的参数个数来自操作数栈，不占用动作记录长度。
        int payloadLen = length;

        Action a;
        a.code = code;
        a.length = payloadLen;
        a.bytePos = recStart;
        a.byteEnd = payloadStart + payloadLen;
        int bodySkip = 0;   // DefineFunction 的函数体位于动作记录之后，需额外跳过

        if (code == 0x96) {  // Push
            a.pushData.assign(data + payloadStart, data + payloadStart + payloadLen);
        } else if (code == 0x88) {  // ConstantPool
            ByteView pb(data + payloadStart, (size_t)payloadLen);
            std::uint16_t n = pb.readUI16();
            for (std::uint16_t k = 0; k < n; ++k) {
                std::string str; int c;
                while ((c = pb.readUI8()) != 0 && c != -1) str.push_back((char)c);
                a.strs.push_back(std::move(str));
            }
        } else if (code == 0x81) {  // GotoFrame（0-based）
            ByteView pb(data + payloadStart, (size_t)payloadLen);
            a.gotoFrame = (int)pb.readUI16();
        } else if (code == 0x83) {  // GetURL
            ByteView pb(data + payloadStart, (size_t)payloadLen);
            { std::string u; int c; while ((c = pb.readUI8()) != 0 && c != -1) u.push_back((char)c); a.str = u; }
            { std::string t; int c; while ((c = pb.readUI8()) != 0 && c != -1) t.push_back((char)c); }
        } else if (code == 0x99 || code == 0x9D) {  // Jump / If
            ByteView pb(data + payloadStart, (size_t)payloadLen);
            std::uint8_t buf[2]; buf[0] = pb.readUI8(); buf[1] = pb.readUI8();
            a.offset = readI16(buf);
        } else if (code == 0x9B || code == 0x8E) {  // DefineFunction / DefineFunction2
            ByteView pb(data + payloadStart, (size_t)payloadLen);
            auto rdStr = [&]() { std::string s; int c; while ((c = pb.readUI8()) != 0 && c != -1) s.push_back((char)c); return s; };
            a.funcName = rdStr();
            std::uint16_t nparams = pb.readUI16();
            int regCount = 0;
            if (code == 0x8E) { regCount = pb.readUI8(); pb.readUI16(); }  // RegisterCount + 16-bit flags
            for (int k = 0; k < nparams && !pb.overflow(); ++k) {
                int reg = 0;
                if (code == 0x8E) reg = pb.readUI8();   // REGISTERPARAM.Register（0=命名变量）
                a.funcParams.push_back(rdStr());
                a.funcParamRegs.push_back(reg);
            }
            std::uint16_t codeSize = pb.readUI16();
            a.funcRegCount = regCount;
            int bodyStart = payloadStart + payloadLen;  // 函数体紧跟动作记录之后
            if (codeSize > 0 && bodyStart + codeSize <= (int)size)
                a.funcBody.assign(data + bodyStart, data + bodyStart + codeSize);
            bodySkip = codeSize;
        } else if (code == 0x8A || code == 0x8B || code == 0x8C || code == 0x8D ||
                   code == 0x8F || code == 0x94 || code == 0x87 || code == 0x9F) {
            // 带 payload 的动作：WaitForFrame/SetTarget/GotoLabel/WaitForFrame2/Try/With/StoreRegister/GotoFrame2
            if (payloadLen > 0) a.raw.assign(data + payloadStart, data + payloadStart + payloadLen);
            if (code == 0x8C) { ByteView pb(data + payloadStart, (size_t)payloadLen); std::string t; int c; while ((c = pb.readUI8()) != 0 && c != -1) t.push_back((char)c); a.str = t; }
        } else {
            // 其余带数据的动作：保存 payload 以便在未知情况下使用
            if (payloadLen > 0) a.raw.assign(data + payloadStart, data + payloadStart + payloadLen);
        }

        s.actions.push_back(std::move(a));
        bv.seek((size_t)(payloadStart + payloadLen + bodySkip));
    }
    return s;
}

// 运行时解析 Push 操作数（依赖常量池与寄存器，均取自当前帧）
static void pushValues(const std::vector<std::uint8_t>& data, std::vector<Value>& stack,
                       const std::vector<Value>& registers, const std::vector<Value>& constantPool) {
    ByteView bv(data.data(), data.size());
    while (bv.pos() < data.size() && !bv.overflow()) {
        std::uint8_t t = bv.readUI8();
        Value v;
        switch (t) {
            case 0: { std::string s; int c; while ((c = bv.readUI8()) != 0 && c != -1) s.push_back((char)c); v = Value(s); } break;
            case 1: { std::uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = bv.readUI8(); v = Value((double)readF32(b)); } break;
            case 2: v = Value::nullV(); break;
            case 3: v = Value::undef(); break;
            case 4: { std::uint8_t r = bv.readUI8(); v = (r < registers.size()) ? registers[r] : Value::undef(); } break;
            case 5: { std::uint8_t x = bv.readUI8(); v = Value(x != 0); } break;
            case 6: {  // double：Flash AVM1 按"高 32 位字在前、每个字小端"组合
                        //（实际 SWF 中 0.5 存为 00 00 E0 3F 00 00 00 00，按标准 64 位小端会读成 ~0）。
                std::uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = bv.readUI8();
                auto word = [](const std::uint8_t* p) -> std::uint32_t {
                    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
                           ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
                };
                std::uint64_t bits = ((std::uint64_t)word(b) << 32) | word(b + 4);
                double dv; std::memcpy(&dv, &bits, 8);
                v = Value(dv); } break;
            case 7: { std::uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = bv.readUI8(); v = Value((double)readI32(b)); } break;
            case 8: { std::uint8_t i = bv.readUI8(); v = (i < constantPool.size()) ? constantPool[i] : Value::undef(); } break;
            case 9: { std::uint16_t i = bv.readUI16(); v = (i < constantPool.size()) ? constantPool[i] : Value::undef(); } break;
            case 10: { std::uint32_t i = bv.readUI32(); v = (i < constantPool.size()) ? constantPool[i] : Value::undef(); } break;
            default: v = Value::undef(); break;
        }
        stack.push_back(v);
    }
}

static Value callClipMethod(ITimeline* clip, const std::string& name, const std::vector<Value>& args) {
    if (!clip) return Value::undef();
    if (name == "gotoAndPlay" || name == "gotoAndStop") {
        int frame = !args.empty() ? (int)args[0].toNumber() : 1;
        if (name == "gotoAndPlay") clip->gotoAndPlay(frame);
        else clip->gotoAndStop(frame);
    } else if (name == "play") clip->play();
    else if (name == "stop") clip->stop();
    else if (name == "nextFrame") clip->nextFrame();
    else if (name == "prevFrame") clip->prevFrame();
    else if (name == "stopAllSounds") clip->stopAllSounds();
    return Value::undef();
}

// 解析帧标签（字符串）到 1-based 帧号，失败返回 -1
static int resolveFrame(Machine& m, const Value& v) {
    if (v.type == Value::T::Number) return (int)v.toNumber();
    if (v.type == Value::T::String) {
        auto it = m.frameLabels.find(v.str);
        if (it != m.frameLabels.end()) return it->second;
    }
    return -1;
}

static Value dispatchMethod(Machine& m, Value object, Value method, const std::vector<Value>& args) {
    std::string mname;
    ITimeline* clip = nullptr;
    IClip* ic = nullptr;
    if (method.isMethod) { mname = method.methodName; clip = (ITimeline*)method.clip; ic = method.iclip; }
    else if (method.type == Value::T::String) mname = method.str;
    if (!clip && !ic && object.isClip) { clip = (ITimeline*)object.clip; ic = object.iclip; }

    // ---- 通用对象：LoadVars.sendAndLoad（本地无网络 → 同步 onLoad(false)）----
    if (object.type == Value::T::Object && object.obj) {
        if (mname == "sendAndLoad") {
            auto it = object.obj->find("onLoad");
            if (it != object.obj->end() && it->second.type == Value::T::Function)
                return m.callFunctionValue(it->second, { Value(false) });
            return Value::undef();
        }
        auto it = object.obj->find(mname);
        if (it != object.obj->end() && it->second.type == Value::T::Function)
            return m.callFunctionValue(it->second, args);
    }
    // ---- String.substr ----
    if (mname == "substr" && object.type == Value::T::String) {
        int start = args.size() > 0 ? (int)args[0].toNumber() : 0;
        int len = args.size() > 1 ? (int)args[1].toNumber() : (int)object.str.size();
        if (start < 0) start = 0;
        return Value(object.str.substr((std::size_t)start, (std::size_t)std::max(0, len)));
    }

    // MovieClip 扩展方法：显示列表运行期增删、拖拽、包围盒、遮罩。
    if (mname == "attachMovie" || mname == "duplicateMovieClip" ||
        mname == "createEmptyMovieClip" || mname == "startDrag" ||
        mname == "stopDrag" || mname == "getBounds" || mname == "setMask") {
        if (ic) return ic->callExt(mname, args);
        if (clip) return clip->callExt(mname, args);
        return Value::undef();
    }

    // 命名实例：时间轴方法作用于该实例的精灵时间轴
    if (ic) {
        if (mname == "hitTest") {
            bool h = ic->hitTest(args.empty() ? Value::undef() : args[0],
                                 args.size() > 1 && args[1].toBool());
            return Value(h);
        }
        if (mname == "gotoAndPlay" || mname == "gotoAndStop" || mname == "play" ||
            mname == "stop" || mname == "nextFrame" || mname == "prevFrame" ||
            mname == "stopAllSounds") {
            ic->callTimeline(mname, args);
            return Value::undef();
        }
        return Value::undef();
    }
    if (!clip) return Value::undef();
    // 帧标签解析（gotoAndPlay/Stop 支持字符串标签）
    if ((mname == "gotoAndPlay" || mname == "gotoAndStop") && !args.empty()) {
        int f = resolveFrame(m, args[0]);
        if (f > 0) {
            if (mname == "gotoAndPlay") clip->gotoAndPlay(f);
            else clip->gotoAndStop(f);
            return Value::undef();
        }
    }
    return callClipMethod(clip, mname, args);
}

// ---------- 解释器 ----------
// 单次调用的执行帧：独立操作数栈/寄存器/常量池/局部作用域，支持函数递归。
struct Frame {
    std::vector<Value> stack;
    std::vector<Value> registers;
    std::vector<Value> constantPool;
    std::vector<Value> localSlots;               // DefineLocal 局部变量槽
    std::map<std::string, Value> scope;          // 函数参数/局部（命名）；根帧为空，用 globals
    const Script* script = nullptr;
    std::shared_ptr<Avm1Function> fn;            // 保持函数体存活
    int pc = 0;
    bool ret = false;
    Value retValue;
    std::map<int, int> posToIdx;
    bool isRoot = false;   // 是否为最外层（根）帧；变量定义/赋值时决定落到 globals 还是作用域
};

// 以两种写法写主时间轴变量表（原样 key + 归一化 key），使脚本混用写法时仍指向同一槽。
static void storeVar(std::map<std::string, Value>& g, const std::string& n, const Value& v) {
    g[n] = v;
    std::string k = rootVarKey(n);
    if (k != n) g[k] = v;
}

// 预置内建全局对象（System 等）
static void ensureBuiltins(std::map<std::string, Value>& globals) {
    if (globals.find("System") == globals.end()) globals["System"] = makeObj("System");
}

// ---------- AVM1 opcode 命令化（M5）----------
// 仿 M4 的 TagRegistry：把 opcode 分发从大 switch 改为「code -> handler」注册表。
// handler 直接操作 Frame（操作数栈/pc/ret）与 Machine（globals/时间轴）；
// 控制流（Jump/If/Return）通过 OpResult 表达：jumped 时设置新 pc，否则主循环自行 ++pc。
struct OpResult { bool jumped = false; int newPc = 0; };
using OpHandler = std::function<OpResult(Machine&, Frame&, const Action&)>;

class OpRegistry {
    std::map<int, OpHandler> m_;
public:
    void add(std::uint8_t code, OpHandler h) { m_[code] = std::move(h); }
    void add(std::initializer_list<std::uint8_t> cs, OpHandler h) { for (auto c : cs) m_[c] = h; }
    OpHandler* find(std::uint8_t code) { auto it = m_.find(code); return it == m_.end() ? nullptr : &it->second; }
};
inline OpRegistry& avm1Ops() { static OpRegistry r; return r; }
inline Value popVF(Frame& F) {   // 空栈保护弹出（与 run 内 popV 等价）
    if (F.stack.empty()) return Value::undef();
    Value v = F.stack.back(); F.stack.pop_back(); return v;
}
// 当前调用栈指针：run 与 callFunctionValue 执行期各自令其指向自己的局部 frames，
// 使 CallFunction handler 能 push 新帧（两处执行器共享同一套 OpRegistry handler）。
static thread_local std::vector<Frame>* g_callStack = nullptr;
// 把所有 AVM1 opcode 注册进 avm1Ops()（M5 渐进迁移：当前仅示例 End，其余仍走 switch）。
void registerAvm1Ops() {
    avm1Ops().add(0x00, [](Machine&, Frame& F, const Action&) -> OpResult {  // End
        F.ret = true; return {};
    });
    // ---- 算术 / 比较 / 逻辑 / 栈：纯操作数栈操作 ----
    avm1Ops().add(0x0A, [](Machine&, Frame& F, const Action&) -> OpResult {  // Add
        Value b=popVF(F); Value c=popVF(F);
        bool str=(b.type==Value::T::String||c.type==Value::T::String);
        F.stack.push_back(str?Value(c.toString()+b.toString()):Value(b.toNumber()+c.toNumber())); return {}; });
    avm1Ops().add(0x0B, [](Machine&, Frame& F, const Action&) -> OpResult {  // Subtract
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toNumber()-b.toNumber())); return {}; });
    avm1Ops().add(0x0C, [](Machine&, Frame& F, const Action&) -> OpResult {  // Multiply
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.toNumber()*c.toNumber())); return {}; });
    avm1Ops().add(0x0D, [](Machine&, Frame& F, const Action&) -> OpResult {  // Divide
        Value b=popVF(F); Value c=popVF(F); double d=b.toNumber();
        F.stack.push_back(Value(d==0?std::nan(""):c.toNumber()/d)); return {}; });
    avm1Ops().add(0x0E, [](Machine&, Frame& F, const Action&) -> OpResult {  // Equals
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.toString()==c.toString())); return {}; });
    avm1Ops().add(0x0F, [](Machine&, Frame& F, const Action&) -> OpResult {  // Less
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toNumber()<b.toNumber())); return {}; });
    avm1Ops().add(0x10, [](Machine&, Frame& F, const Action&) -> OpResult {  // And
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.toBool()&&c.toBool())); return {}; });
    avm1Ops().add(0x11, [](Machine&, Frame& F, const Action&) -> OpResult {  // Or
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.toBool()||c.toBool())); return {}; });
    avm1Ops().add(0x12, [](Machine&, Frame& F, const Action&) -> OpResult {  // Not
        Value b=popVF(F); F.stack.push_back(Value(!b.toBool())); return {}; });
    avm1Ops().add(0x13, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringEquals
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.toString()==c.toString())); return {}; });
    avm1Ops().add(0x14, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringLength
        Value b=popVF(F); F.stack.push_back(Value((double)b.toString().size())); return {}; });
    avm1Ops().add({0x15,0x35}, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringExtract/MBStringExtract
        Value cntV=popVF(F); Value idxV=popVF(F); Value strV=popVF(F);
        std::string s=strV.toString(); int i=(int)idxV.toNumber(); int n=(int)cntV.toNumber();
        std::string sub=(i>=1&&i<=(int)s.size())?s.substr((size_t)(i-1),(size_t)std::max(0,n)):"";
        F.stack.push_back(Value(sub)); return {}; });
    avm1Ops().add(0x17, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); return {}; });  // Pop
    avm1Ops().add(0x18, [](Machine&, Frame& F, const Action&) -> OpResult {  // ToInteger
        Value b=popVF(F); F.stack.push_back(Value(std::floor(b.toNumber()))); return {}; });
    // ---- 纯栈 / 定义辅助组（第二批）----
    avm1Ops().add(0x21, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringAdd
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toString()+b.toString())); return {}; });
    avm1Ops().add(0x26, [](Machine&, Frame& F, const Action&) -> OpResult {  // Trace
        Value b=popVF(F); std::fprintf(stderr, "[AVM1 Trace] %s\n", b.toString().c_str()); return {}; });
    avm1Ops().add(0x29, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringLess
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toString()<b.toString())); return {}; });
    avm1Ops().add(0x2A, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); return {}; });  // Throw
    avm1Ops().add(0x2B, [](Machine&, Frame& F, const Action&) -> OpResult {  // CastOp
        popVF(F); Value v=popVF(F); F.stack.push_back(v); return {}; });
    avm1Ops().add(0x2C, [](Machine&, Frame& F, const Action&) -> OpResult {  // ImplementsOp
        popVF(F); Value v=popVF(F); F.stack.push_back(v); return {}; });
    avm1Ops().add(0x30, [](Machine&, Frame& F, const Action&) -> OpResult {  // RandomNumber
        Value b=popVF(F); int mx=(int)b.toNumber(); F.stack.push_back(Value((double)(mx>0?std::rand()%mx:0))); return {}; });
    avm1Ops().add(0x31, [](Machine&, Frame& F, const Action&) -> OpResult {  // MBStringLength
        Value b=popVF(F); F.stack.push_back(Value((double)b.toString().size())); return {}; });
    avm1Ops().add({0x32,0x36}, [](Machine&, Frame& F, const Action&) -> OpResult {  // CharToAscii/MBCharToAscii
        Value b=popVF(F); std::string s=b.toString(); F.stack.push_back(Value((double)(s.empty()?0:(unsigned char)s[0]))); return {}; });
    avm1Ops().add({0x33,0x37}, [](Machine&, Frame& F, const Action&) -> OpResult {  // AsciiToChar/MBAsciiToChar
        Value b=popVF(F); int c=(int)b.toNumber(); F.stack.push_back(Value(std::string(1,(char)c))); return {}; });
    avm1Ops().add(0x34, [](Machine&, Frame& F, const Action&) -> OpResult {  // GetTime
        F.stack.push_back(Value((double)((std::uint32_t)std::clock()))); return {}; });
    avm1Ops().add(0x3A, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); F.stack.push_back(Value(true)); return {}; });  // Delete
    avm1Ops().add(0x3B, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); popVF(F); F.stack.push_back(Value(true)); return {}; });  // Delete2
    avm1Ops().add(0x4A, [](Machine&, Frame& F, const Action&) -> OpResult {  // ToNumber
        Value b=popVF(F); F.stack.push_back(Value(b.toNumber())); return {}; });
    avm1Ops().add(0x4B, [](Machine&, Frame& F, const Action&) -> OpResult {  // ToString
        Value b=popVF(F); F.stack.push_back(Value(b.toString())); return {}; });
    avm1Ops().add(0x50, [](Machine&, Frame& F, const Action&) -> OpResult {  // Increment
        Value b=popVF(F); F.stack.push_back(Value(b.toNumber()+1)); return {}; });
    avm1Ops().add(0x51, [](Machine&, Frame& F, const Action&) -> OpResult {  // Decrement
        Value b=popVF(F); F.stack.push_back(Value(b.toNumber()-1)); return {}; });
    avm1Ops().add(0x66, [](Machine&, Frame& F, const Action&) -> OpResult {  // StrictEquals
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(b.type==c.type && b.toString()==c.toString())); return {}; });
    avm1Ops().add(0x67, [](Machine&, Frame& F, const Action&) -> OpResult {  // Greater
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toNumber()>b.toNumber())); return {}; });
    avm1Ops().add(0x68, [](Machine&, Frame& F, const Action&) -> OpResult {  // StringGreater
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toString()>b.toString())); return {}; });
    // ---- 定义 / 属性组（第三批）----
    avm1Ops().add(0x3F, [](Machine&, Frame& F, const Action&) -> OpResult {  // Modulo
        Value b=popVF(F); Value c=popVF(F); double d=b.toNumber();
        F.stack.push_back(Value(d==0?std::nan(""):std::fmod(c.toNumber(),d))); return {}; });
    avm1Ops().add(0x40, [](Machine&, Frame& F, const Action&) -> OpResult {  // NewObject
        Value name=popVF(F); Value nargsV=popVF(F); int nargs=(int)nargsV.toNumber();
        if(nargs<0)nargs=0; for(int k=0;k<nargs && !F.stack.empty();++k) popVF(F);
        F.stack.push_back(makeObj(name.toString())); return {}; });
    avm1Ops().add(0x42, [](Machine&, Frame& F, const Action&) -> OpResult {  // InitArray
        Value nV=popVF(F); int n=(int)nV.toNumber(); if(n<0)n=0;
        Value o=makeObj("Array"); std::vector<Value> elems;
        for(int k=0;k<n && !F.stack.empty();++k) elems.push_back(popVF(F));
        for(int k=0;k<(int)elems.size();++k) (*o.obj)[std::to_string(k)]=elems[elems.size()-1-k];
        F.stack.push_back(o); return {}; });
    avm1Ops().add(0x43, [](Machine&, Frame& F, const Action&) -> OpResult {  // InitObject
        Value nV=popVF(F); int n=(int)nV.toNumber(); if(n<0)n=0;
        Value o=makeObj("Object"); std::vector<std::pair<std::string,Value>> kv;
        for(int k=0;k<n && F.stack.size()>=2;++k){ Value v=popVF(F); Value nm=popVF(F); kv.push_back({nm.toString(),v}); }
        for(auto& p:kv) (*o.obj)[p.first]=p.second; F.stack.push_back(o); return {}; });
    avm1Ops().add(0x44, [](Machine&, Frame& F, const Action&) -> OpResult {  // TypeOf
        Value v=popVF(F); std::string t;
        if(v.isClip) t="movieclip";
        else switch(v.type){
            case Value::T::Number: t="number"; break;
            case Value::T::Bool: t="boolean"; break;
            case Value::T::String: t="string"; break;
            case Value::T::Function: t="function"; break;
            case Value::T::Null: t="null"; break;
            case Value::T::Undefined: t="undefined"; break;
            default: t="object"; break;
        }
        F.stack.push_back(Value(t)); return {}; });
    avm1Ops().add(0x45, [](Machine& m, Frame& F, const Action&) -> OpResult {  // TargetPath
        Value v=popVF(F); std::string p;
        if(v.isClip) p = v.iclip ? v.iclip->getPath() : m.timeline->targetPath();
        F.stack.push_back(Value(p)); return {}; });
    avm1Ops().add({0x46,0x55}, [](Machine&, Frame& F, const Action&) -> OpResult {  // Enumerate/Enumerate2
        popVF(F); F.stack.push_back(Value::nullV()); return {}; });
    avm1Ops().add(0x47, [](Machine&, Frame& F, const Action&) -> OpResult {  // Add2
        Value b=popVF(F); Value c=popVF(F); bool str=(b.type==Value::T::String||c.type==Value::T::String);
        F.stack.push_back(str?Value(c.toString()+b.toString()):Value(b.toNumber()+c.toNumber())); return {}; });
    avm1Ops().add(0x48, [](Machine&, Frame& F, const Action&) -> OpResult {  // Less2
        Value b=popVF(F); Value c=popVF(F); F.stack.push_back(Value(c.toNumber()<b.toNumber())); return {}; });
    avm1Ops().add(0x49, [](Machine&, Frame& F, const Action&) -> OpResult {  // Equals2
        Value b=popVF(F); Value c=popVF(F); bool eq;
        if(b.type==Value::T::Number && c.type==Value::T::Number) eq=(b.num==c.num);
        else if(b.type==Value::T::String || c.type==Value::T::String) eq=(b.toString()==c.toString());
        else eq=(b.toNumber()==c.toNumber());
        F.stack.push_back(Value(eq)); return {}; });
    avm1Ops().add(0x4C, [](Machine&, Frame& F, const Action&) -> OpResult {  // PushDuplicate
        if(!F.stack.empty()) F.stack.push_back(F.stack.back()); return {}; });
    avm1Ops().add(0x4D, [](Machine&, Frame& F, const Action&) -> OpResult {  // StackSwap
        if(F.stack.size()>=2){ Value t=F.stack.back(); F.stack.back()=*(F.stack.end()-2); *(F.stack.end()-2)=t; } return {}; });
    avm1Ops().add(0x52, [](Machine& m, Frame& F, const Action&) -> OpResult {  // CallMethod
        Value method=popVF(F); Value object=popVF(F); Value nargsV=popVF(F);
        int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
        std::vector<Value> args;
        for(int k=0;k<nargs && !F.stack.empty();++k){ args.push_back(F.stack.back()); F.stack.pop_back(); }
        F.stack.push_back(dispatchMethod(m, object, method, args)); return {}; });
    avm1Ops().add(0x53, [](Machine&, Frame& F, const Action&) -> OpResult {  // NewMethod
        Value method=popVF(F); Value object=popVF(F); Value nargsV=popVF(F);
        int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
        for(int k=0;k<nargs && !F.stack.empty();++k) popVF(F);
        F.stack.push_back(makeObj(method.toString())); return {}; });
    avm1Ops().add(0x54, [](Machine&, Frame& F, const Action&) -> OpResult {  // InstanceOf
        popVF(F); popVF(F); F.stack.push_back(Value(false)); return {}; });
    avm1Ops().add({0x60,0x61,0x62,0x63,0x64,0x65}, [](Machine&, Frame& F, const Action& a) -> OpResult {  // BitOps
        Value b=popVF(F); Value c=popVF(F);
        std::int64_t x=(std::int64_t)b.toNumber(), y=(std::int64_t)c.toNumber();
        std::int64_t r=0;
        switch(a.code){
            case 0x60: r=x&y; break;
            case 0x61: r=x|y; break;
            case 0x62: r=x^y; break;
            case 0x63: r=x<<(y&63); break;
            case 0x64: r=x>>(y&63); break;
            case 0x65: r=((std::uint64_t)x)>>(y&63); break;
        }
        F.stack.push_back(Value((double)(std::int32_t)r)); return {}; });
    avm1Ops().add(0x69, [](Machine&, Frame& F, const Action&) -> OpResult {  // Extends
        popVF(F); Value v=popVF(F); F.stack.push_back(v); return {}; });
    avm1Ops().add(0x87, [](Machine&, Frame& F, const Action& a) -> OpResult {  // StoreRegister
        Value v=popVF(F); std::uint8_t r = a.raw.empty()?0:a.raw[0];
        if(r>=(int)F.registers.size()) F.registers.resize(r+1);
        F.registers[r]=v; return {}; });
    avm1Ops().add(0x88, [](Machine&, Frame& F, const Action& a) -> OpResult {  // ConstantPool
        F.constantPool.clear(); for(auto& str:a.strs) F.constantPool.push_back(Value(str)); return {}; });
    avm1Ops().add(0x96, [](Machine&, Frame& F, const Action& a) -> OpResult {  // Push
        pushValues(a.pushData, F.stack, F.registers, F.constantPool); return {}; });
    // ---- 变量 / 成员组（第四批）----
    avm1Ops().add(0x1C, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value name=popVF(F); Value vv;
        if(name.type==Value::T::String){ std::string n=name.str;
            if(n.find('.')!=std::string::npos && n!="."){
                if(auto* sob=(IClip*)m.timeline->resolveClipPath(n)){ F.stack.push_back(makeIClip(sob)); return {}; }
                if(resolveTrailingPath(m.timeline,n,vv)){ F.stack.push_back(vv); return {}; } }
            if(n=="this"){ F.stack.push_back(m.currentThis?makeIClip(m.currentThis):(m.currentParent?makeIClip(m.currentParent):makeClip(m.currentClip?m.currentClip:m.timeline))); }
            else if(n=="_root"||n=="_global"||n=="_level0"){ F.stack.push_back(makeClip(m.timeline)); }
            else if(n=="_parent"){ F.stack.push_back(m.currentParent?makeIClip(m.currentParent):Value::undef()); }
            else if(isRootVarName(n)){ auto* p=findVar(m.globals,n); F.stack.push_back(p?*p:Value::undef()); }
            else { auto sit=F.scope.find(n);
                if(sit!=F.scope.end()) F.stack.push_back(sit->second);
                else if(clipVarGet(m,n,vv)){ F.stack.push_back(vv); }
                else { auto* p=findVar(m.globals,n); F.stack.push_back(p?*p:Value::undef()); } } }
        else F.stack.push_back(Value::undef()); return {}; });
    avm1Ops().add(0x1D, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value val=popVF(F); Value name=popVF(F);
        if(name.type==Value::T::String){
            if(isRootVarName(name.str)) storeVar(m.globals,name.str,val);
            else if(m.currentThis){ int pid=clipPropByName(name.str); if(pid>=0) m.currentThis->setNumberProp(pid,val.toNumber()); else clipVarSet(m,name.str,val); }
            else if(F.isRoot) storeVar(m.globals,name.str,val);
            else F.scope[name.str]=val; } return {}; });
    avm1Ops().add(0x3C, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value val=popVF(F); Value name=popVF(F);
        if(name.type==Value::T::String){
            if(m.currentThis) clipVarSet(m,name.str,val);
            else if(F.isRoot) storeVar(m.globals,name.str,val);
            else F.scope[name.str]=val; } return {}; });
    avm1Ops().add(0x41, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value name=popVF(F);
        if(name.type==Value::T::String){
            if(m.currentThis) clipVarSet(m,name.str,Value::undef());
            else if(F.isRoot) storeVar(m.globals,name.str,Value::undef());
            else F.scope[name.str]=Value::undef(); } return {}; });
    avm1Ops().add(0x4E, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value name=popVF(F); Value obj=popVF(F);
        if(obj.isClip && name.type==Value::T::String){ std::string n=name.str;
            if(obj.iclip){
                if(n=="gotoAndPlay"||n=="gotoAndStop"||n=="play"||n=="stop"||n=="nextFrame"||n=="prevFrame"||n=="stopAllSounds"||n=="hitTest"||n=="attachMovie"||n=="duplicateMovieClip"||n=="createEmptyMovieClip"||n=="startDrag"||n=="stopDrag"||n=="getBounds"||n=="setMask"){ Value mm; mm.type=Value::T::Object; mm.isMethod=true; mm.methodName=n; mm.iclip=obj.iclip; F.stack.push_back(mm); return {}; }
                if(n=="_parent"){ void* p=obj.iclip->parentClip(); F.stack.push_back(p?makeIClip((IClip*)p):makeClip(m.timeline)); return {}; }
                if(n=="_root"||n=="_level0"||n=="_global"){ F.stack.push_back(makeClip(m.timeline)); return {}; }
                int pid=clipPropByName(n); Value out; if(pid>=0 && iclipGetProp(obj.iclip,pid,out)){ F.stack.push_back(out); return {}; }
                if(void* ch=obj.iclip->findChildClip(n)){ F.stack.push_back(makeIClip((IClip*)ch)); return {}; }
                auto it=obj.iclip->members->find(n); if(it!=obj.iclip->members->end()){ F.stack.push_back(it->second); return {}; }
                F.stack.push_back(Value::undef()); return {}; }
            ITimeline* t=(ITimeline*)obj.clip;
            if(n=="_root"){ F.stack.push_back(makeClip(m.timeline)); return {}; }
            if(n=="_currentframe"||n=="_currentFrame"){ F.stack.push_back(Value((double)t->currentFrame())); return {}; }
            if(n=="_totalframes"||n=="_totalFrames"){ F.stack.push_back(Value((double)t->totalFrames())); return {}; }
            if(n=="_framesloaded"){ F.stack.push_back(Value((double)t->framesLoaded())); return {}; }
            if(n=="_parent"){ F.stack.push_back(makeClip(t)); return {}; }
            if(n=="_x"||n=="_y"||n=="_width"||n=="_height"||n=="_rotation"){ F.stack.push_back(Value(0.0)); return {}; }
            if(n=="_xscale"||n=="_yscale"||n=="_alpha"){ F.stack.push_back(Value(100.0)); return {}; }
            if(n=="_visible"){ F.stack.push_back(Value(true)); return {}; }
            if(n=="_name"){ F.stack.push_back(Value(t->clipName())); return {}; }
            if(n=="_target"){ F.stack.push_back(Value(t->targetPath())); return {}; }
            // _root 上按实例名取子剪辑：GetVariable 只处理带点号的完整路径，
            // 而 AVM1 的 _root.bonus.gotoAndPlay(2) 是先 GetMember(_root,"bonus")。
            // 解析不到会让整条调用链静默失效（钩爪回收后无法触发复位）。
            if(!n.empty() && n.find('.')==std::string::npos && n[0]!='/'){
                std::string cp="_root."+n;
                if(void* ch=t->resolveClipPath(cp)){ F.stack.push_back(makeIClip((IClip*)ch)); return {}; }
            }
            { auto* gp=findVar(m.globals,n); if(gp){ F.stack.push_back(*gp); return {}; } }
            F.stack.push_back(Value::undef()); return {}; }
        if(obj.type==Value::T::Object && obj.obj){ auto it=obj.obj->find(name.toString()); if(it!=obj.obj->end()){ F.stack.push_back(it->second); return {}; } }
        if(name.type==Value::T::String && name.str=="length" && obj.type==Value::T::String){ F.stack.push_back(Value((double)obj.str.size())); return {}; }
        F.stack.push_back(Value::undef()); return {}; });
    avm1Ops().add(0x4F, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value val=popVF(F); Value name=popVF(F); Value obj=popVF(F);
        if(obj.iclip && name.type==Value::T::String){ int pid=clipPropByName(name.str);
            if(pid>=0){ obj.iclip->setNumberProp(pid, val.toNumber()); return {}; }
            (*obj.iclip->members)[name.str]=val; return {}; }
        if(obj.isClip && name.type==Value::T::String){ storeVar(m.globals,name.str,val); return {}; }
        if(obj.type==Value::T::Object && obj.obj){ (*obj.obj)[name.toString()]=val; } return {}; });
    // ---- 时间轴控制 ----
    avm1Ops().add(0x04, [](Machine& m, Frame&, const Action&) -> OpResult { (m.currentClip?m.currentClip:m.timeline)->nextFrame(); return {}; });
    avm1Ops().add(0x05, [](Machine& m, Frame&, const Action&) -> OpResult { (m.currentClip?m.currentClip:m.timeline)->prevFrame(); return {}; });
    avm1Ops().add(0x06, [](Machine& m, Frame&, const Action&) -> OpResult { (m.currentClip?m.currentClip:m.timeline)->play(); return {}; });
    avm1Ops().add(0x07, [](Machine& m, Frame&, const Action&) -> OpResult { (m.currentClip?m.currentClip:m.timeline)->stop(); return {}; });
    avm1Ops().add(0x08, [](Machine&, Frame&, const Action&) -> OpResult { return {}; });  // ToggleQuality 忽略
    avm1Ops().add(0x09, [](Machine& m, Frame&, const Action&) -> OpResult { m.timeline->stopAllSounds(); return {}; });
    avm1Ops().add(0x81, [](Machine& m, Frame&, const Action& a) -> OpResult { (m.currentClip?m.currentClip:m.timeline)->gotoAndStop(a.gotoFrame + 1); return {}; });
    avm1Ops().add(0x8C, [](Machine&, Frame&, const Action&) -> OpResult { return {}; });  // GotoLabel 忽略
    avm1Ops().add(0x9F, [](Machine& m, Frame& F, const Action& a) -> OpResult {
        bool playFlag=(a.raw.empty()?false:(a.raw[0]&1)!=0); Value fr=popVF(F); int f=resolveFrame(m,fr);
        if(f>0){ auto* CL=m.currentClip?m.currentClip:m.timeline; if(playFlag) CL->gotoAndPlay(f); else CL->gotoAndStop(f); } return {}; });
    // ---- 短桩 ----
    avm1Ops().add(0x20, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); return {}; });  // SetTarget2
    avm1Ops().add(0x22, [](Machine& m, Frame& F, const Action&) -> OpResult {
        Value idx=popVF(F); Value tgt=popVF(F); int i=(int)idx.toNumber();
        ITimeline* cl=(tgt.isClip)?(ITimeline*)tgt.clip:(m.currentClip?m.currentClip:m.timeline); Value out=Value::undef();
        switch(i){ case 0: case 1: out=Value(0.0); break; case 2: case 3: out=Value(100.0); break;
            case 4: out=Value((double)cl->currentFrame()); break; case 5: out=Value((double)cl->totalFrames()); break;
            case 6: out=Value(100.0); break; case 7: out=Value(true); break;
            case 8: case 9: case 10: out=Value(0.0); break;
            case 11: out=Value(cl->targetPath()); break; case 12: out=Value((double)cl->framesLoaded()); break;
            case 13: out=Value(cl->clipName()); break; default: out=Value::undef(); break; }
        F.stack.push_back(out); return {}; });
    avm1Ops().add(0x23, [](Machine&, Frame& F, const Action&) -> OpResult {
        Value val=popVF(F); Value idx=popVF(F); Value tgt=popVF(F);
        if(tgt.iclip){ int i=(int)idx.toNumber(); tgt.iclip->setNumberProp(i, val.toNumber()); } return {}; });
    avm1Ops().add({0x24,0x25,0x27,0x28}, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); popVF(F); popVF(F); return {}; });  // CloneSprite/RemoveSprite/StartDrag/EndDrag
    avm1Ops().add(0x83, [](Machine&, Frame&, const Action&) -> OpResult { return {}; });  // GetURL 忽略
    avm1Ops().add(0x8B, [](Machine&, Frame& F, const Action&) -> OpResult { popVF(F); return {}; });  // SetTarget
    avm1Ops().add(0x9A, [](Machine&, Frame&, const Action&) -> OpResult { return {}; });  // GetURL2 忽略
    // ---- 控制流（最后）----
    avm1Ops().add(0x3D, [](Machine& m, Frame& F, const Action&) -> OpResult {  // CallFunction
        Value name=popVF(F); Value nargsV=popVF(F);
        int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
        std::vector<Value> args;
        for(int k=0;k<nargs && !F.stack.empty();++k){ args.push_back(F.stack.back()); F.stack.pop_back(); }
        Value fnv;
        if(name.type==Value::T::String){
            auto sit=F.scope.find(name.str);
            if(sit!=F.scope.end()) fnv=sit->second;
            else { auto git=m.globals.find(name.str); if(git!=m.globals.end()) fnv=git->second; }
        }
        if(fnv.type==Value::T::Function && fnv.func && g_callStack->size()<(size_t)m.maxCallDepth){
            ++F.pc;
            g_callStack->push_back(Frame{});
            Frame& nf=*g_callStack->rbegin();
            nf.fn=fnv.func; nf.script=&fnv.func->body;
            for(size_t i=0;i<fnv.func->body.actions.size();++i) nf.posToIdx[fnv.func->body.actions[i].bytePos]=(int)i;
            nf.registers.resize(fnv.func->regCount>0?fnv.func->regCount:0);
            for(size_t i=0;i<fnv.func->params.size();++i){
                Value arg=(i<args.size())?args[i]:Value::undef();
                int reg=(i<fnv.func->paramRegs.size())?fnv.func->paramRegs[i]:0;
                if(reg>0 && reg<(int)nf.registers.size()) nf.registers[reg]=arg;
                else nf.scope[fnv.func->params[i]]=arg;
            }
            return {true, F.pc};
        }
        F.stack.push_back(Value::undef()); return {}; });
    avm1Ops().add(0x3E, [](Machine&, Frame& F, const Action&) -> OpResult { F.retValue=popVF(F); F.ret=true; return {}; });
    avm1Ops().add(0x99, [](Machine&, Frame& F, const Action& a) -> OpResult {
        auto it=F.posToIdx.find(a.byteEnd+a.offset);
        if(it!=F.posToIdx.end()) return {true, it->second};
        F.ret=true; return {}; });
    avm1Ops().add(0x9D, [](Machine&, Frame& F, const Action& a) -> OpResult {
        Value cond=popVF(F);
        if(cond.toBool()){ auto it=F.posToIdx.find(a.byteEnd+a.offset); if(it!=F.posToIdx.end()) return {true, it->second}; F.ret=true; return {}; }
        return {}; });
    avm1Ops().add({0x9B,0x8E}, [](Machine& m, Frame& F, const Action& a) -> OpResult {
        auto fn2=std::make_shared<Avm1Function>();
        fn2->name=a.funcName; fn2->params=a.funcParams; fn2->paramRegs=a.funcParamRegs;
        fn2->regCount=a.funcRegCount; fn2->body=disassemble(a.funcBody.data(), a.funcBody.size());
        Value fv; fv.type=Value::T::Function; fv.func=fn2;
        F.stack.push_back(fv);
        if(!a.funcName.empty()){
            if(F.isRoot) storeVar(m.globals, a.funcName, fv);   // 顶层函数入全局，支持全局/递归调用
            else F.scope[a.funcName]=fv;
        } return {}; });
    avm1Ops().add(0x9E, [](Machine&, Frame&, const Action&) -> OpResult { return {}; });  // Call（忽略）
}

void Machine::run(const Script& s) {
    if (!timeline || s.actions.empty()) return;
    ensureBuiltins(globals);

    std::vector<Frame> frames;
    frames.push_back(Frame{});
    { Frame& f0 = frames.back(); f0.script = &s; f0.isRoot = true;
      for (size_t i = 0; i < s.actions.size(); ++i) f0.posToIdx[s.actions[i].bytePos] = (int)i; }
    g_callStack = &frames;

    // 统一执行预算：后向跳转 / 递归调用此前没有任何上限，会永久占用主线程。
    std::size_t steps = 0;
    while (!frames.empty()) {
        if (++steps > 2000000) break;
        Frame& F = frames.back();
        // 帧结束（Return/End/越界）：弹出并把返回值交给调用方
        if (F.ret || F.pc < 0 || F.pc >= (int)F.script->actions.size()) {
            Value rv = F.retValue;
            frames.pop_back();
            if (!frames.empty()) frames.back().stack.push_back(rv);
            continue;
        }
        std::vector<Value>& stack = F.stack;
        // 空栈保护弹出：AVM1（尤其混淆过的）常见"弹空栈"，语义上得到 undefined。
        auto popV = [&stack]() -> Value {
            if (stack.empty()) return Value::undef();
            Value v = stack.back(); stack.erase(stack.end() - 1); return v;
        };
        std::vector<Value>& registers = F.registers;
        std::vector<Value>& constantPool = F.constantPool;
        int& pc = F.pc;
        bool& ret = F.ret;
        std::map<int,int>& posToIdx = F.posToIdx;
        bool isRoot = (frames.size() == 1);
        // 无目标的播放控制作用于"当前剪辑"：执行精灵帧脚本时即该精灵，否则主时间轴。
        ITimeline* CL = currentClip ? currentClip : timeline;
        const Action& a = F.script->actions[pc];
        bool jumped = false;
        {   // M5 注册表优先分派；未注册 opcode 仍走下方 switch（渐进迁移）
            static const bool inited = (registerAvm1Ops(), true);
            if (auto* oh = avm1Ops().find(a.code)) {
                OpResult r = (*oh)(*this, F, a);
                if (r.jumped) F.pc = r.newPc; else ++F.pc;
                continue;
            }
        }
        switch (a.code) { default: break; }
        if (!jumped) ++pc;
    }
}

// ---------- Machine 运行时辅助 ----------
// 以独立调用栈执行函数值（事件处理器 / onEnterFrame / LoadVars.onLoad 等）。
Value Machine::callFunctionValue(const Value& v, const std::vector<Value>& args) {
    if (v.type != Value::T::Function || !v.func) return Value::undef();
    static thread_local int s_depth = 0;
    if (s_depth > maxCallDepth) return Value::undef();

    std::vector<Frame> frames;
    frames.emplace_back();
    {
        Frame& f0 = frames.back();
        f0.script = &v.func->body;
        f0.registers.resize(v.func->regCount > 0 ? v.func->regCount : 0);
        for (size_t i = 0; i < v.func->params.size(); ++i) {
            Value arg = (i < args.size()) ? args[i] : Value::undef();
            int reg = (i < v.func->paramRegs.size()) ? v.func->paramRegs[i] : 0;
            if (reg > 0 && reg < (int)f0.registers.size()) f0.registers[reg] = arg;
            else f0.scope[v.func->params[i]] = arg;
        }
        for (size_t i = 0; i < f0.script->actions.size(); ++i)
            f0.posToIdx[f0.script->actions[i].bytePos] = (int)i;
    }
    std::vector<Frame>* saveCS = g_callStack;
    g_callStack = &frames;
    ++s_depth;
    Value result;
    while (!frames.empty()) {
        Frame& F = frames.back();
        if (F.ret || F.pc < 0 || F.pc >= (int)F.script->actions.size()) {
            Value rv = F.retValue;
            frames.pop_back();
            if (!frames.empty()) frames.back().stack.push_back(rv);
            else result = rv;
            continue;
        }
        const Action& a = F.script->actions[F.pc];
        if (auto* oh = avm1Ops().find(a.code)) {
            OpResult r = (*oh)(*this, F, a);
            if (r.jumped) F.pc = r.newPc; else ++F.pc;
            continue;
        }
        ++F.pc;  // 未注册 opcode：跳过（与原 default: break 语义一致）
    }
    --s_depth;
    g_callStack = saveCS;
    return result;
}

void Machine::callHandler(const std::string& handlerName, const std::vector<Value>& args) {
    // 根/globals 上的处理器（_root.onEnterFrame = function(){} 语义）
    auto it = globals.find(handlerName);
    if (it != globals.end() && it->second.type == Value::T::Function) {
        callFunctionValue(it->second, args);
    }
}

}  // namespace avm1
}  // namespace swf
