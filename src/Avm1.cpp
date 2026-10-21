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

void Machine::run(const Script& s) {
    if (!timeline || s.actions.empty()) return;
    ensureBuiltins(globals);

    std::vector<Frame> frames;
    frames.push_back(Frame{});
    { Frame& f0 = frames.back(); f0.script = &s;
      for (size_t i = 0; i < s.actions.size(); ++i) f0.posToIdx[s.actions[i].bytePos] = (int)i; }

    while (!frames.empty()) {
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
        switch (a.code) {
            // ---- 时间轴控制 ----
            case 0x00: ret = true; break;                          // End
            case 0x04: CL->nextFrame(); break;                     // NextFrame
            case 0x05: CL->prevFrame(); break;                     // PrevFrame
            case 0x06: CL->play(); break;                    // Play
            case 0x07: CL->stop(); break;                    // Stop
            case 0x08: break;                                      // ToggleQuality（忽略）
            case 0x09: timeline->stopAllSounds(); break;           // StopSounds
            // ---- 算术 / 比较 / 逻辑 ----
            case 0x0A: { Value b=popV(); Value c=popV();           // Add
                         bool str=(b.type==Value::T::String||c.type==Value::T::String);
                         stack.push_back(str?Value(c.toString()+b.toString())
                                            :Value(b.toNumber()+c.toNumber())); } break;
            case 0x0B: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()-b.toNumber())); } break;  // Subtract
            case 0x0C: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toNumber()*c.toNumber())); } break;  // Multiply
            case 0x0D: { Value b=popV(); Value c=popV(); double d=b.toNumber();
                         stack.push_back(Value(d==0?std::nan(""):c.toNumber()/d)); } break;                          // Divide
            case 0x0E: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toString()==c.toString())); } break; // Equals
            case 0x0F: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()<b.toNumber())); } break;  // Less
            case 0x10: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toBool()&&c.toBool())); } break;     // And
            case 0x11: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toBool()||c.toBool())); } break;     // Or
            case 0x12: { Value b=popV(); stack.push_back(Value(!b.toBool())); } break;                                // Not
            case 0x13: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toString()==c.toString())); } break; // StringEquals
            case 0x14: { Value b=popV(); stack.push_back(Value((double)b.toString().size())); } break;                // StringLength
            case 0x15: case 0x35: {                                // StringExtract / MBStringExtract
                         Value cntV=popV(); Value idxV=popV(); Value strV=popV();
                         std::string s=strV.toString(); int i=(int)idxV.toNumber(); int n=(int)cntV.toNumber();
                         std::string sub=(i>=1&&i<=(int)s.size())?s.substr((size_t)(i-1),(size_t)std::max(0,n)):"";
                         stack.push_back(Value(sub)); } break;
            case 0x17: popV(); break;                              // Pop
            case 0x18: { Value b=popV(); stack.push_back(Value(std::floor(b.toNumber()))); } break;  // ToInteger
            // ---- 变量 ----
            case 0x1C: { Value name=popV(); Value vv;             // GetVariable
                         if(name.type==Value::T::String){
                             std::string n=name.str;
                             // 带点路径（如 _root.T4.ob.price）：GetVariable 会做路径解析得到剪辑对象。
                             if(n.find('.')!=std::string::npos && n!="."){
                                 if(auto* sob = (IClip*)timeline->resolveClipPath(n)){ stack.push_back(makeIClip(sob)); break; }
                                 Value pv;
                                 if(resolveTrailingPath(timeline, n, pv)){ stack.push_back(pv); break; }
                             }
                             if(n=="this"){ stack.push_back(currentThis?makeIClip(currentThis):(currentParent?makeIClip(currentParent):makeClip(currentClip?currentClip:timeline))); }
                             else if(n=="_root"||n=="_global"||n=="_level0"){ stack.push_back(makeClip(timeline)); }
                             else if(n=="_parent"){ stack.push_back(currentParent?makeIClip(currentParent):Value::undef()); }
                             else if(isRootVarName(n)){ auto* p=findVar(globals,n); stack.push_back(p?*p:Value::undef()); }
                             else {
                                 auto sit=F.scope.find(n);
                                 if(sit!=F.scope.end()) stack.push_back(sit->second);
                                 else if(clipVarGet(*this,n,vv)){ stack.push_back(vv); }
                                 else { auto* p=findVar(globals,n); stack.push_back(p?*p:Value::undef()); }
                             }
                         } else stack.push_back(Value::undef()); } break;
            case 0x1D: { Value val=popV(); Value name=popV();      // SetVariable（先弹 value 再弹 name）
                         if(name.type==Value::T::String){
                             if(isRootVarName(name.str)) storeVar(globals,name.str,val);
                             else if(isRoot && !currentThis) storeVar(globals,name.str,val);
                             else if(currentThis) { int pid=clipPropByName(name.str);
                                 if(pid>=0){ double d=(name.str=="_target"||name.str=="_name")?0:val.toNumber(); currentThis->setNumberProp(pid,d); }
                                 else clipVarSet(*this,name.str,val); }
                             else F.scope[name.str]=val;
                         } } break;
            case 0x20: popV(); break;                              // SetTarget2（忽略）
            case 0x21: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toString()+b.toString())); } break;  // StringAdd
            case 0x22: { Value idx=popV(); Value tgt=popV();       // GetProperty
                         int i=(int)idx.toNumber(); Value out=Value::undef();
                         if (tgt.iclip) {
                             if (!iclipGetProp(tgt.iclip, i, out)) out = Value::undef();
                         } else {
                             // 目标不是剪辑对象（例如空串 ""）时，Flash 语义指向"当前时间轴"
                             // （正在执行脚本的剪辑）；主时间轴脚本的 currentClip 为空 → 主时间轴。
                             ITimeline* cl=(tgt.isClip)?(ITimeline*)tgt.clip:(currentClip?currentClip:timeline);
                             switch(i){
                                 case 0: case 1: out=Value(0.0); break;                          // _x/_y
                                 case 2: case 3: out=Value(100.0); break;                        // _xscale/_yscale
                                 case 4: out=Value((double)cl->currentFrame()); break;           // _currentframe
                                 case 5: out=Value((double)cl->totalFrames()); break;            // _totalframes
                                 case 6: out=Value(100.0); break;                                // _alpha
                                 case 7: out=Value(true); break;                                 // _visible
                                 case 8: case 9: case 10: out=Value(0.0); break;                 // _width/_height/_rotation
                                 case 11: out=Value(cl->targetPath()); break;                    // _target
                                 case 12: out=Value((double)cl->framesLoaded()); break;          // _framesloaded
                                 case 13: out=Value(cl->clipName()); break;                      // _name
                                 default: out=Value::undef(); break;
                             }
                         }
                         stack.push_back(out); } break;
            case 0x23: { Value val=popV(); Value idx=popV(); Value tgt=popV();   // SetProperty
                         if (tgt.iclip) { int i=(int)idx.toNumber(); tgt.iclip->setNumberProp(i, val.toNumber()); } } break;
            case 0x24: case 0x25: case 0x27: case 0x28:            // CloneSprite/RemoveSprite/StartDrag/EndDrag
                popV(); popV(); popV(); break;
            case 0x26: { Value b=popV(); std::fprintf(stderr, "[AVM1 Trace] %s\n", b.toString().c_str()); } break;
            case 0x29: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toString()<b.toString())); } break; // StringLess
            case 0x2A: popV(); break;                              // Throw（忽略）
            case 0x2B: { popV(); Value v=popV(); stack.push_back(v); } break;   // CastOp
            case 0x2C: { popV(); Value v=popV(); stack.push_back(v); } break;   // ImplementsOp
            case 0x30: { Value b=popV(); int mx=(int)b.toNumber();
                         stack.push_back(Value((double)(mx>0?std::rand()%mx:0))); } break;   // RandomNumber
            case 0x31: { Value b=popV(); stack.push_back(Value((double)b.toString().size())); } break;  // MBStringLength
            case 0x32: case 0x36: { Value b=popV(); std::string s=b.toString();              // CharToAscii/MBCharToAscii
                         stack.push_back(Value((double)(s.empty()?0:(unsigned char)s[0]))); } break;
            case 0x33: case 0x37: { Value b=popV(); int c=(int)b.toNumber();                 // AsciiToChar/MBAsciiToChar
                         stack.push_back(Value(std::string(1,(char)c))); } break;
            case 0x34: stack.push_back(Value((double)((std::uint32_t)std::clock()))); break; // GetTime
            case 0x3A: popV(); stack.push_back(Value(true)); break;   // Delete
            case 0x3B: popV(); popV(); stack.push_back(Value(true)); break;  // Delete2
            case 0x3C: { Value val=popV(); Value name=popV();         // DefineLocal（先弹 value 再弹 name）
                         if(name.type==Value::T::String){
                             if(currentThis) clipVarSet(*this,name.str,val);
                             else if(isRoot) storeVar(globals,name.str,val);
                             else F.scope[name.str]=val; } } break;
            case 0x41: { Value name=popV();                           // DefineLocal2（仅名字，值 undefined）
                         if(name.type==Value::T::String){
                             if(currentThis) clipVarSet(*this,name.str,Value::undef());
                             else if(isRoot) storeVar(globals,name.str,Value::undef());
                             else F.scope[name.str]=Value::undef(); } } break;
            case 0x3D: { // CallFunction：pop name, pop nargs, pop 参数, 调用, push 返回值
                         Value name=popV(); Value nargsV=popV();
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         std::vector<Value> args;
                         for(int k=0;k<nargs && !stack.empty();++k){ args.push_back(stack.back()); stack.erase(stack.end()-1); }
                         Value fnv;
                         if(name.type==Value::T::String){
                             auto sit=F.scope.find(name.str);
                             if(sit!=F.scope.end()) fnv=sit->second;
                             else { auto git=globals.find(name.str); if(git!=globals.end()) fnv=git->second; }
                         }
                         if(fnv.type==Value::T::Function && fnv.func && frames.size()<(size_t)maxCallDepth){
                             ++pc;  // 返回后从下一条继续
                             frames.push_back(Frame{});
                             Frame& nf=frames.back();
                             nf.fn=fnv.func; nf.script=&fnv.func->body;
                             for(size_t i=0;i<fnv.func->body.actions.size();++i) nf.posToIdx[fnv.func->body.actions[i].bytePos]=(int)i;
                             nf.registers.resize(fnv.func->regCount>0?fnv.func->regCount:0);
                             for(size_t i=0;i<fnv.func->params.size();++i){
                                 Value arg=(i<args.size())?args[i]:Value::undef();
                                 int reg=(i<fnv.func->paramRegs.size())?fnv.func->paramRegs[i]:0;
                                 if(reg>0 && reg<(int)nf.registers.size()) nf.registers[reg]=arg;
                                 else nf.scope[fnv.func->params[i]]=arg;
                             }
                             jumped=true;
                         } else {
                             stack.push_back(Value::undef());
                         } } break;
            case 0x3E: { F.retValue = popV(); ret=true; } break;      // Return
            case 0x3F: { Value b=popV(); Value c=popV(); double d=b.toNumber();    // Modulo
                         stack.push_back(Value(d==0?std::nan(""):std::fmod(c.toNumber(),d))); } break;
            case 0x40: { Value name=popV(); Value nargsV=popV();      // NewObject
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         for(int k=0;k<nargs && !stack.empty();++k) popV();
                         stack.push_back(makeObj(name.toString())); } break;
            case 0x42: { Value nV=popV(); int n=(int)nV.toNumber(); if(n<0)n=0;   // InitArray
                         Value o=makeObj("Array");
                         std::vector<Value> elems;
                         for(int k=0;k<n && !stack.empty();++k) elems.push_back(popV());
                         for(int k=0;k<(int)elems.size();++k) (*o.obj)[std::to_string(k)]=elems[elems.size()-1-k];
                         stack.push_back(o); } break;
            case 0x43: { Value nV=popV(); int n=(int)nV.toNumber(); if(n<0)n=0;   // InitObject
                         Value o=makeObj("Object");
                         std::vector<std::pair<std::string,Value>> kv;
                         for(int k=0;k<n && stack.size()>=2;++k){ Value v=popV(); Value nm=popV(); kv.push_back({nm.toString(),v}); }
                         for(auto& p:kv) (*o.obj)[p.first]=p.second;
                         stack.push_back(o); } break;
            case 0x44: { Value v=popV(); std::string t;               // TypeOf
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
                         stack.push_back(Value(t)); } break;
            case 0x45: { Value v=popV(); std::string p;               // TargetPath
                         if(v.isClip) p = v.iclip ? v.iclip->getPath() : timeline->targetPath();
                         stack.push_back(Value(p)); } break;
            case 0x46: case 0x55: popV(); stack.push_back(Value::nullV()); break;  // Enumerate/Enumerate2
            case 0x47: { Value b=popV(); Value c=popV();              // Add2
                         bool str=(b.type==Value::T::String||c.type==Value::T::String);
                         stack.push_back(str?Value(c.toString()+b.toString())
                                            :Value(b.toNumber()+c.toNumber())); } break;
            case 0x48: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()<b.toNumber())); } break;  // Less2
            case 0x49: { Value b=popV(); Value c=popV();              // Equals2
                         bool eq;
                         if(b.type==Value::T::Number && c.type==Value::T::Number) eq=(b.num==c.num);
                         else if(b.type==Value::T::String || c.type==Value::T::String) eq=(b.toString()==c.toString());
                         else eq=(b.toNumber()==c.toNumber());
                         stack.push_back(Value(eq)); } break;
            case 0x4A: { Value b=popV(); stack.push_back(Value(b.toNumber())); } break;   // ToNumber
            case 0x4B: { Value b=popV(); stack.push_back(Value(b.toString())); } break;   // ToString
            case 0x4C: if(!stack.empty()) stack.push_back(stack.back()); break;           // PushDuplicate
            case 0x4D: if(stack.size()>=2){ Value t=stack.back(); stack.back()=*(stack.end()-2); *(stack.end()-2)=t; } break;  // StackSwap
            case 0x4E: { Value name=popV(); Value obj=popV();         // GetMember
                         if(obj.isClip && name.type==Value::T::String){
                             std::string n=name.str;
                             if (obj.iclip) {
                                 if(n=="gotoAndPlay"||n=="gotoAndStop"||n=="play"||n=="stop"||
                                    n=="nextFrame"||n=="prevFrame"||n=="stopAllSounds"||n=="hitTest"){
                                     Value m; m.type=Value::T::Object; m.isMethod=true;
                                     m.methodName=n; m.iclip=obj.iclip; stack.push_back(m); break;
                                 }
                                 // 剪辑对象的父子/根引用（AS1/AS2 中 `clip._parent`、`clip._root` 很常用）。
                                 if(n=="_parent"){ void* p=obj.iclip->parentClip();
                                                  stack.push_back(p?makeIClip((IClip*)p):makeClip(timeline)); break; }
                                 if(n=="_root"||n=="_level0"||n=="_global"){ stack.push_back(makeClip(timeline)); break; }
                                 int pid=clipPropByName(n);
                                 if(pid>=0){ Value out; if(!iclipGetProp(obj.iclip,pid,out)) out=Value::undef();
                                             stack.push_back(out); break; }
                                 if (void* ch = obj.iclip->findChildClip(n)) { stack.push_back(makeIClip((IClip*)ch)); break; }
                                 { auto it=obj.iclip->members->find(n);
                                   if(it!=obj.iclip->members->end()){ stack.push_back(it->second); break; } }
                                 stack.push_back(Value::undef()); break;
                             }
                             if(n=="gotoAndPlay"||n=="gotoAndStop"||n=="play"||n=="stop"||n=="nextFrame"||n=="prevFrame"||n=="stopAllSounds"){
                                 Value m; m.type=Value::T::Object; m.isMethod=true; m.methodName=n; m.clip=obj.clip; stack.push_back(m); break;
                             }
                             if (void* ch = ((ITimeline*)obj.clip)->findChildClip(n)) { stack.push_back(makeIClip((IClip*)ch)); break; }
                             ITimeline* t=(ITimeline*)obj.clip;
                             if(n=="_currentframe"||n=="_currentFrame"){stack.push_back(Value((double)t->currentFrame()));break;}
                             if(n=="_totalframes"||n=="_totalFrames"){stack.push_back(Value((double)t->totalFrames()));break;}
                             if(n=="_framesloaded"){stack.push_back(Value((double)t->framesLoaded()));break;}
                             if(n=="_root"){stack.push_back(makeClip(timeline));break;}
                             if(n=="_parent"){stack.push_back(makeClip(t));break;}
                             if(n=="_x"||n=="_y"||n=="_width"||n=="_height"||n=="_rotation"){stack.push_back(Value(0.0));break;}
                             if(n=="_xscale"||n=="_yscale"||n=="_alpha"){stack.push_back(Value(100.0));break;}
                             if(n=="_visible"){stack.push_back(Value(true));break;}
                             if(n=="_name"){stack.push_back(Value(t->clipName()));break;}
                             if(n=="_target"){stack.push_back(Value(t->targetPath()));break;}
                             { auto* gp=findVar(globals,n); if(gp){ stack.push_back(*gp); break; } }
                             stack.push_back(Value::undef()); break;
                         }
                         if(obj.type==Value::T::Object && obj.obj){ auto it=obj.obj->find(name.toString()); if(it!=obj.obj->end()){stack.push_back(it->second);break;} }
                         if(name.type==Value::T::String && name.str=="length" && obj.type==Value::T::String)
                             { stack.push_back(Value((double)obj.str.size())); break; }
                         stack.push_back(Value::undef()); } break;
            case 0x4F: { Value val=popV(); Value name=popV(); Value obj=popV();  // SetMember
                         if(obj.iclip && name.type==Value::T::String){
                             int pid=clipPropByName(name.str);
                             if(pid>=0){ obj.iclip->setNumberProp(pid, val.toNumber()); break; }
                             (*obj.iclip->members)[name.str]=val; break;
                         }
                         if(obj.isClip && name.type==Value::T::String){ storeVar(globals,name.str,val); break; }
                         if(obj.type==Value::T::Object && obj.obj){ (*obj.obj)[name.toString()]=val; } } break;
            case 0x50: { Value b=popV(); stack.push_back(Value(b.toNumber()+1)); } break;   // Increment
            case 0x51: { Value b=popV(); stack.push_back(Value(b.toNumber()-1)); } break;   // Decrement
            case 0x52: { // CallMethod：pop method, pop object, pop nargs, pop args（自顶向下）
                         Value method=popV(); Value object=popV(); Value nargsV=popV();
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         std::vector<Value> args;
                         for(int k=0;k<nargs && !stack.empty();++k){ args.push_back(stack.back()); stack.erase(stack.end()-1); }
                         stack.push_back(dispatchMethod(*this, object, method, args)); } break;
            case 0x53: { // NewMethod
                         Value method=popV(); Value object=popV(); Value nargsV=popV();
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         for(int k=0;k<nargs && !stack.empty();++k) popV();
                         stack.push_back(makeObj(method.toString())); } break;
            case 0x54: { popV(); popV(); stack.push_back(Value(false)); } break;   // InstanceOf
            case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: {   // 位运算
                         Value b=popV(); Value c=popV();
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
                         stack.push_back(Value((double)(std::int32_t)r)); } break;
            case 0x66: { Value b=popV(); Value c=popV();
                         stack.push_back(Value(b.type==c.type && b.toString()==c.toString()));} break;  // StrictEquals
            case 0x67: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()>b.toNumber())); } break;  // Greater
            case 0x68: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toString()>b.toString())); } break;  // StringGreater
            case 0x69: { popV(); Value v=popV(); stack.push_back(v); } break;   // Extends
            case 0x87: { Value v=popV(); std::uint8_t r = a.raw.empty()?0:a.raw[0];   // StoreRegister
                         if(r>=(int)registers.size()) registers.resize(r+1);
                         registers[r]=v; } break;
            case 0x88: constantPool.clear(); for(auto& str:a.strs) constantPool.push_back(Value(str)); break;  // ConstantPool
            case 0x96: pushValues(a.pushData, stack, registers, constantPool); break;   // Push
            case 0x81: CL->gotoAndStop(a.gotoFrame + 1); break;   // GotoFrame（0-based → 1-based）
            case 0x83: break;   // GetURL（无网络）
            case 0x8C: { int f = resolveFrame(*this, Value(a.str)); if(f>0) CL->gotoAndPlay(f); } break;  // GotoLabel
            case 0x8B: break;   // SetTarget
            case 0x9A: { popV(); popV(); } break;   // GetURL2：pop SendVars 方法 + URL（无网络导航）
            case 0x99: { auto it=posToIdx.find(a.byteEnd + a.offset);   // Jump
                         if(it!=posToIdx.end()) { pc=it->second; jumped=true; } else ret=true; } break;
            case 0x9D: { Value cond=popV();                             // If
                         if(cond.toBool()){ auto it=posToIdx.find(a.byteEnd + a.offset);
                             if(it!=posToIdx.end()) { pc=it->second; jumped=true; } else ret=true; } } break;
            case 0x9F: { bool playFlag = (a.raw.empty()?false:(a.raw[0]&1)!=0);   // GotoFrame2
                         Value fr=popV(); int f=resolveFrame(*this, fr);
                         if(f>0){ if(playFlag) CL->gotoAndPlay(f); else CL->gotoAndStop(f); } } break;
            case 0x9B: case 0x8E: { // DefineFunction[2]：构造函数值压栈；具名则绑定到当前作用域
                         auto fn=std::make_shared<Avm1Function>();
                         fn->name=a.funcName; fn->params=a.funcParams; fn->paramRegs=a.funcParamRegs; fn->regCount=a.funcRegCount;
                         fn->body=disassemble(a.funcBody.data(), a.funcBody.size());
                         Value fv; fv.type=Value::T::Function; fv.func=fn;
                         stack.push_back(fv);
                         if(!a.funcName.empty()){ if(isRoot) globals[a.funcName]=fv; else F.scope[a.funcName]=fv; } } break;
            case 0x9E: popV(); break;   // Call（帧子程序）：弹帧引用，暂不执行目标帧
            default:
                // 未知动作：忽略（不破坏栈，避免误弹）
                break;
        }
        if (!jumped) ++pc;
    }
}

// ---------- Machine 运行时辅助 ----------
// 以独立调用栈执行函数值（事件处理器 / onEnterFrame / LoadVars.onLoad 等）。
Value Machine::callFunctionValue(const Value& v, const std::vector<Value>& args) {
    if (v.type != Value::T::Function || !v.func) return Value::undef();
    static thread_local int s_depth = 0;
    if (s_depth > maxCallDepth) return Value::undef();

    struct LFrame {
        const Script* script = nullptr;
        std::vector<Value> stack;
        std::vector<Value> registers;
        std::vector<Value> constantPool;
        std::map<std::string, Value> scope;
        std::map<int, int> posToIdx;
        int pc = 0; bool ret = false; Value retValue;
    };
    std::vector<LFrame> frames;
    frames.emplace_back();
    {
        LFrame& f0 = frames.back();
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
    ++s_depth;
    Value result;
    while (!frames.empty()) {
        LFrame& F = frames.back();
        if (F.ret || F.pc < 0 || F.pc >= (int)F.script->actions.size()) {
            Value rv = F.retValue;
            frames.pop_back();
            if (!frames.empty()) frames.back().stack.push_back(rv);
            else result = rv;
            continue;
        }
        std::vector<Value>& stack = F.stack;
        auto popV = [&stack]() -> Value {
            if (stack.empty()) return Value::undef();
            Value v = stack.back(); stack.erase(stack.end() - 1); return v;
        };
        int& pc = F.pc;
        bool jumped = false;
        // 无目标的播放控制作用于"当前剪辑"：执行精灵帧脚本时即该精灵，否则主时间轴。
        ITimeline* CL = currentClip ? currentClip : timeline;
        const Action& a = F.script->actions[pc];
        switch (a.code) {
            case 0x00: F.ret = true; break;
            case 0x0A: { Value b=popV(); Value c=popV();
                         bool str=(b.type==Value::T::String||c.type==Value::T::String);
                         stack.push_back(str?Value(c.toString()+b.toString()):Value(b.toNumber()+c.toNumber()));} break;
            case 0x0B: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()-b.toNumber()));} break;
            case 0x0C: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toNumber()*c.toNumber()));} break;
            case 0x0D: { Value b=popV(); Value c=popV(); double d=b.toNumber(); stack.push_back(Value(d==0?std::nan(""):c.toNumber()/d));} break;
            case 0x0E: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toString()==c.toString()));} break;
            case 0x0F: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()<b.toNumber()));} break;
            case 0x10: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toBool()&&c.toBool()));} break;
            case 0x11: { Value b=popV(); Value c=popV(); stack.push_back(Value(b.toBool()||c.toBool()));} break;
            case 0x12: { Value b=popV(); stack.push_back(Value(!b.toBool()));} break;   // Not
            case 0x14: { Value b=popV(); stack.push_back(Value((double)b.toString().size()));} break;
            case 0x17: popV(); break;   // Pop
            case 0x1C: { Value name=popV(); Value vv;   // GetVariable
                         if(name.type==Value::T::String){
                             std::string n=name.str;
                             if(n.find('.')!=std::string::npos && n!="."){
                                 if(auto* sob = (IClip*)timeline->resolveClipPath(n)){ stack.push_back(makeIClip(sob)); break; }
                                 Value pv;
                                 if(resolveTrailingPath(timeline, n, pv)){ stack.push_back(pv); break; }
                             }
                             if(n=="this"){ stack.push_back(currentThis?makeIClip(currentThis):(currentParent?makeIClip(currentParent):makeClip(currentClip?currentClip:timeline))); }
                             else if(n=="_root"||n=="_global"||n=="_level0"){ stack.push_back(makeClip(timeline)); }
                             else if(n=="_parent"){ stack.push_back(currentParent?makeIClip(currentParent):Value::undef()); }
                             else if(isRootVarName(n)){ auto* p=findVar(globals,n); stack.push_back(p?*p:Value::undef()); }
                             else { auto sit=F.scope.find(n);
                                 if(sit!=F.scope.end()) stack.push_back(sit->second);
                                 else if(clipVarGet(*this,n,vv)){ stack.push_back(vv); }
                                 else { auto* p=findVar(globals,n); stack.push_back(p?*p:Value::undef()); } }
                         } else stack.push_back(Value::undef()); } break;
            case 0x1D: { Value val=popV(); Value name=popV();   // SetVariable
                         if(name.type==Value::T::String){
                             if(isRootVarName(name.str)) storeVar(globals,name.str,val);
                             else if(currentThis){ int pid=clipPropByName(name.str);
                                 if(pid>=0) currentThis->setNumberProp(pid,val.toNumber());
                                 else clipVarSet(*this,name.str,val);
                             } else F.scope[name.str]=val;
                         } } break;
            case 0x21: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toString()+b.toString()));} break;
            case 0x22: { Value idx=popV(); Value tgt=popV(); int i=(int)idx.toNumber();
                         ITimeline* cl=(tgt.isClip)?(ITimeline*)tgt.clip:(currentClip?currentClip:timeline); Value out=Value::undef();
                         switch(i){ case 0: case 1: out=Value(0.0); break; case 2: case 3: out=Value(100.0); break;
                             case 4: out=Value((double)cl->currentFrame()); break; case 5: out=Value((double)cl->totalFrames()); break;
                             case 12: out=Value((double)cl->framesLoaded()); break; case 11: out=Value(cl->targetPath()); break;
                             default: out=Value::undef(); break; }
                         stack.push_back(out); } break;
            case 0x26: { Value b=popV(); std::fprintf(stderr, "[AVM1 Trace] %s\n", b.toString().c_str()); } break;
            case 0x2A: popV(); break;
            case 0x30: { Value b=popV(); int mx=(int)b.toNumber(); stack.push_back(Value((double)(mx>0?std::rand()%mx:0)));} break;
            case 0x3D: { // CallFunction
                         Value name=popV(); Value nargsV=popV();
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         std::vector<Value> fargs;
                         for(int k=0;k<nargs && !stack.empty();++k){ fargs.push_back(stack.back()); stack.erase(stack.end()-1); }
                         Value fnv;
                         if(name.type==Value::T::String){
                             auto sit=F.scope.find(name.str);
                             if(sit!=F.scope.end()) fnv=sit->second;
                             else { auto git=globals.find(name.str); if(git!=globals.end()) fnv=git->second; }
                         }
                         if(fnv.type==Value::T::Function && fnv.func && frames.size()<(size_t)maxCallDepth){
                             ++pc;
                             frames.emplace_back();
                             LFrame& nf=frames.back();
                             nf.script=&fnv.func->body;
                             nf.registers.resize(fnv.func->regCount>0?fnv.func->regCount:0);
                             for(size_t i=0;i<fnv.func->params.size();++i){
                                 Value arg=(i<fargs.size())?fargs[i]:Value::undef();
                                 int reg=(i<fnv.func->paramRegs.size())?fnv.func->paramRegs[i]:0;
                                 if(reg>0 && reg<(int)nf.registers.size()) nf.registers[reg]=arg;
                                 else nf.scope[fnv.func->params[i]]=arg;
                             }
                             for(size_t i=0;i<nf.script->actions.size();++i)
                                 nf.posToIdx[nf.script->actions[i].bytePos]=(int)i;
                             jumped=true;
                         } else stack.push_back(Value::undef()); } break;
            case 0x3E: { F.retValue = popV(); F.ret=true; } break;
            case 0x3C: { Value val=popV(); Value name=popV(); if(name.type==Value::T::String) F.scope[name.str]=val; } break;  // DefineLocal
            case 0x40: { Value name=popV(); Value nargsV=popV();   // NewObject
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         for(int k=0;k<nargs && !stack.empty();++k) popV();
                         stack.push_back(makeObj(name.toString())); } break;
            case 0x47: { Value b=popV(); Value c=popV();
                         bool str=(b.type==Value::T::String||c.type==Value::T::String);
                         stack.push_back(str?Value(c.toString()+b.toString()):Value(b.toNumber()+c.toNumber()));} break;
            case 0x48: { Value b=popV(); Value c=popV(); stack.push_back(Value(c.toNumber()<b.toNumber()));} break;
            case 0x49: { Value b=popV(); Value c=popV();
                         bool eq;
                         if(b.type==Value::T::Number && c.type==Value::T::Number) eq=(b.num==c.num);
                         else if(b.type==Value::T::String || c.type==Value::T::String) eq=(b.toString()==c.toString());
                         else eq=(b.toNumber()==c.toNumber());
                         stack.push_back(Value(eq));} break;
            case 0x4A: { Value b=popV(); stack.push_back(Value(b.toNumber()));} break;
            case 0x4B: { Value b=popV(); stack.push_back(Value(b.toString()));} break;
            case 0x4C: if(!stack.empty()) stack.push_back(stack.back()); break;
            case 0x4D: if(stack.size()>=2){ Value t=stack.back(); stack.back()=*(stack.end()-2); *(stack.end()-2)=t; } break;
            case 0x4E: { Value name=popV(); Value obj=popV();   // GetMember
                         if(obj.isClip && name.type==Value::T::String){
                             std::string n=name.str;
                             if (obj.iclip){
                                 if(n=="gotoAndPlay"||n=="gotoAndStop"||n=="play"||n=="stop"||
                                    n=="nextFrame"||n=="prevFrame"||n=="stopAllSounds"||n=="hitTest"){
                                     Value m; m.type=Value::T::Object; m.isMethod=true;
                                     m.methodName=n; m.iclip=obj.iclip; stack.push_back(m); break;
                                 }
                                 int pid=clipPropByName(n); Value out; if(pid>=0 && iclipGetProp(obj.iclip,pid,out)){ stack.push_back(out); break; }
                                 if (void* ch = obj.iclip->findChildClip(n)) { stack.push_back(makeIClip((IClip*)ch)); break; }
                                 auto it=obj.iclip->members->find(n); if(it!=obj.iclip->members->end()){stack.push_back(it->second);break;}
                                 stack.push_back(Value::undef()); break; }
                             ITimeline* t=(ITimeline*)obj.clip;
                             if(n=="_root"){stack.push_back(makeClip(timeline));break;}
                             if(n=="_currentframe"){stack.push_back(Value((double)t->currentFrame()));break;}
                             { auto* gp=findVar(globals,n); if(gp){ stack.push_back(*gp); break; } }
                             stack.push_back(Value::undef()); break;
                         }
                         if(obj.type==Value::T::Object && obj.obj){ auto it=obj.obj->find(name.toString()); if(it!=obj.obj->end()){stack.push_back(it->second);break;} }
                         if(obj.type==Value::T::String && name.type==Value::T::String && name.str=="length"){ stack.push_back(Value((double)obj.str.size())); break; }
                         stack.push_back(Value::undef()); } break;
            case 0x4F: { Value val=popV(); Value name=popV(); Value obj=popV();   // SetMember
                         if(obj.iclip && name.type==Value::T::String){ int pid=clipPropByName(name.str);
                             if(pid>=0){ obj.iclip->setNumberProp(pid, val.toNumber()); break; } (*obj.iclip->members)[name.str]=val; break; }
                         if(obj.isClip && name.type==Value::T::String){ storeVar(globals,name.str,val); break; }
                         if(obj.type==Value::T::Object && obj.obj){ (*obj.obj)[name.toString()]=val; } } break;
            case 0x50: { Value b=popV(); stack.push_back(Value(b.toNumber()+1));} break;
            case 0x51: { Value b=popV(); stack.push_back(Value(b.toNumber()-1));} break;
            case 0x52: { Value method=popV(); Value object=popV(); Value nargsV=popV();   // CallMethod
                         int nargs=(int)nargsV.toNumber(); if(nargs<0)nargs=0;
                         std::vector<Value> args;
                         for(int k=0;k<nargs && !stack.empty();++k){ args.push_back(stack.back()); stack.erase(stack.end()-1); }
                         stack.push_back(dispatchMethod(*this, object, method, args)); } break;
            case 0x81: CL->gotoAndStop(a.gotoFrame + 1); break;
            case 0x88: F.constantPool.clear(); for(auto& str:a.strs) F.constantPool.push_back(Value(str)); break;
            case 0x96: pushValues(a.pushData, stack, F.registers, F.constantPool); break;
            case 0x99: { auto it=F.posToIdx.find(a.byteEnd + a.offset);
                         if(it!=F.posToIdx.end()){ pc=it->second; jumped=true; } else F.ret=true; } break;
            case 0x9D: { Value cond=popV();
                         if(cond.toBool()){ auto it=F.posToIdx.find(a.byteEnd + a.offset);
                             if(it!=F.posToIdx.end()){ pc=it->second; jumped=true; } else F.ret=true; } } break;
            case 0x9F: { bool playFlag=(a.raw.empty()?false:(a.raw[0]&1)!=0); Value fr=popV(); int f=resolveFrame(*this,fr);
                         if(f>0){ if(playFlag) CL->gotoAndPlay(f); else CL->gotoAndStop(f); } } break;
            case 0x9B: case 0x8E: { // DefineFunction[2]
                         auto fn2=std::make_shared<Avm1Function>();
                         fn2->name=a.funcName; fn2->params=a.funcParams; fn2->paramRegs=a.funcParamRegs;
                         fn2->regCount=a.funcRegCount; fn2->body=disassemble(a.funcBody.data(), a.funcBody.size());
                         Value fv; fv.type=Value::T::Function; fv.func=fn2;
                         stack.push_back(fv);
                         if(!a.funcName.empty()) F.scope[a.funcName]=fv; } break;
            default: break;   // 未覆盖动作：忽略（事件处理器多为简单赋值/调用）
        }
        if (!jumped) ++pc;
    }
    --s_depth;
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
