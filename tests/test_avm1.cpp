// AVM1 解释器单元测试（CTest: swfparser_avm1）。
// 用手工构造的动作字节流验证：算术/字符串/比较、SetVariable、跳转/条件跳转、
// 时间轴控制（_root.gotoAndStop）、GetProperty、DefineLocal、DefineFunction 占位、
// GetURL2 / InstanceOf 等栈效果。断言基于 Machine.globals（公开）与 DummyTimeline 状态。
#include "swf/Avm1.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace swf::avm1;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

struct DummyTimeline : ITimeline {
    int frame = 1, total = 5, lastGoto = -1; bool playing = true;
    void gotoAndPlay(int f) override { lastGoto = f; playing = true; frame = f; }
    void gotoAndStop(int f) override { lastGoto = f; playing = false; frame = f; }
    void play() override { playing = true; }
    void stop() override { playing = false; }
    void nextFrame() override { ++frame; }
    void prevFrame() override { --frame; }
    void stopAllSounds() override {}
    int currentFrame() const override { return frame; }
    int totalFrames() const override { return total; }
    int framesLoaded() const override { return total; }
};

// ---- 动作字节流构造助手 ----
static void pushStr(std::vector<std::uint8_t>& b, const std::string& s) {
    b.push_back(0x96);
    std::size_t lenPos = b.size(); b.push_back(0); b.push_back(0);
    b.push_back(0);                                   // type 0 = 字符串
    for (char c : s) b.push_back((std::uint8_t)c);
    b.push_back(0);                                   // 结尾 NUL
    int len = (int)(b.size() - lenPos - 2);
    b[lenPos] = (std::uint8_t)(len & 0xFF); b[lenPos + 1] = (std::uint8_t)((len >> 8) & 0xFF);
}
static void pushInt(std::vector<std::uint8_t>& b, int v) {
    b.push_back(0x96); b.push_back(0x05); b.push_back(0x00);   // len=5
    b.push_back(7);                                            // type 7 = int32
    b.push_back((std::uint8_t)(v & 0xFF)); b.push_back((std::uint8_t)((v >> 8) & 0xFF));
    b.push_back((std::uint8_t)((v >> 16) & 0xFF)); b.push_back((std::uint8_t)((v >> 24) & 0xFF));
}
static void op(std::vector<std::uint8_t>& b, std::uint8_t code) { b.push_back(code); }
// 扩展动作（code>=0x80）：code + UI16 长度 + payload
static void opExt(std::vector<std::uint8_t>& b, std::uint8_t code, const std::vector<std::uint8_t>& payload) {
    b.push_back(code);
    b.push_back((std::uint8_t)(payload.size() & 0xFF));
    b.push_back((std::uint8_t)((payload.size() >> 8) & 0xFF));
    for (auto c : payload) b.push_back(c);
}
// BranchIf(0x9D)：扩展动作，offset 相对记录末尾，跳过 off 字节
static void branchIf(std::vector<std::uint8_t>& b, int off) {
    opExt(b, 0x9D, { (std::uint8_t)(off & 0xFF), (std::uint8_t)((off >> 8) & 0xFF) });
}
// 构造 DefineFunction(0x9B) 记录：Name + NumParams + 参数名 + codeSize + 函数体（体在记录之后）
static void defineFunc(std::vector<std::uint8_t>& b, const std::string& name,
                       const std::vector<std::string>& params, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> p;
    for (char c : name) p.push_back((std::uint8_t)c); p.push_back(0);
    p.push_back((std::uint8_t)(params.size() & 0xFF)); p.push_back(0);
    for (auto& pn : params) { for (char c : pn) p.push_back((std::uint8_t)c); p.push_back(0); }
    p.push_back((std::uint8_t)(body.size() & 0xFF)); p.push_back((std::uint8_t)((body.size() >> 8) & 0xFF));
    opExt(b, 0x9B, p);
    for (auto x : body) b.push_back(x);
}

int main() {
    DummyTimeline tl;

    // 1) 算术 + SetVariable： globals["r"] = 5+3
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "r"); pushInt(b, 5); pushInt(b, 3); op(b, 0x0A); op(b, 0x1D);  // Add; SetVariable
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("r");
        check(it != m.globals.end() && it->second.toNumber() == 8.0, "arithmetic 5+3 == 8 via SetVariable");
    }
    // 2) 字符串拼接（StringAdd 0x21）
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "s"); pushStr(b, "foo"); pushStr(b, "bar"); op(b, 0x21); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("s");
        check(it != m.globals.end() && it->second.toString() == "foobar", "StringAdd 'foo'+'bar' == 'foobar'");
    }
    // 3) GetProperty： _totalframes(5) of _root
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "x");
        pushStr(b, "_root"); op(b, 0x1C);   // GetVariable -> clip
        pushInt(b, 5); op(b, 0x22);         // GetProperty(5=_totalframes)
        op(b, 0x1D);                        // SetVariable x
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("x");
        check(it != m.globals.end() && it->second.toNumber() == 5.0, "getProperty(_root, _totalframes) == 5");
    }
    // 4) 时间轴控制： _root.gotoAndStop(2)
    {
        std::vector<std::uint8_t> b;
        // CallMethod 栈布局（自底向上）：arg…, nargs, object, method（顶）。
        pushInt(b, 2);                              // arg
        pushInt(b, 1);                              // nargs
        pushStr(b, "_root"); op(b, 0x1C);           // object（clip）
        pushStr(b, "gotoAndStop");                  // method（顶）
        op(b, 0x52);                                // CallMethod
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        check(tl.lastGoto == 2 && tl.playing == false, "_root.gotoAndStop(2) drives timeline");
    }
    // 5) 条件跳转（BranchIf 取真，跳过“else”段）
    {
        std::vector<std::uint8_t> b;
        pushInt(b, 1);                       // 条件=真
        std::vector<std::uint8_t> skipped;    // 被跳过的段：globals["skipped"]=1
        pushStr(skipped, "skipped"); pushInt(skipped, 1); op(skipped, 0x1D);
        branchIf(b, (int)skipped.size());
        for (auto c : skipped) b.push_back(c);
        pushStr(b, "reached"); pushInt(b, 1); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        check(m.globals.count("reached") == 1 && m.globals.count("skipped") == 0, "BranchIf(true) skips else-branch");
    }
    // 6) 条件跳转取假（执行“else”段）
    {
        std::vector<std::uint8_t> b;
        pushInt(b, 0);                       // 条件=假
        std::vector<std::uint8_t> kept;
        pushStr(kept, "kept"); pushInt(kept, 1); op(kept, 0x1D);
        branchIf(b, (int)kept.size());
        for (auto c : kept) b.push_back(c);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        check(m.globals.count("kept") == 1, "BranchIf(false) executes branch body");
    }
    // 7) DefineLocal（栈平衡）+ 后续 SetVariable 正常
    {
        std::vector<std::uint8_t> b;
        pushInt(b, 42); op(b, 0x3C);         // DefineLocal 弹出 42 存入局部槽
        pushStr(b, "y"); pushInt(b, 7); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("y");
        check(it != m.globals.end() && it->second.toNumber() == 7.0, "DefineLocal keeps stack balanced");
    }
    // 8) DefineFunction 占位 + SetVariable（模拟 function foo(){}：先 Push 名字，再 DefineFunction）
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "foo");
        opExt(b, 0x9B, { 0, 0, 0, 0 });      // DefineFunction：paramCount=0, codeSize=0
        op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        check(m.globals.count("foo") == 1, "DefineFunction placeholder stored via SetVariable");
    }
    // 9) GetURL2（栈平衡）
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "http://example"); pushStr(b, "GET");
        opExt(b, 0x9A, {});                  // GetURL2 扩展动作，len=0，弹 2
        pushStr(b, "z"); pushInt(b, 9); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("z");
        check(it != m.globals.end() && it->second.toNumber() == 9.0, "GetURL2 keeps stack balanced");
    }
    // 10) InstanceOf（栈效果：弹2 推 false）
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "w"); pushInt(b, 1); pushStr(b, "X"); op(b, 0x54); op(b, 0x1D);  // InstanceOf; SetVariable
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("w");
        check(it != m.globals.end() && it->second.type == Value::T::Bool && it->second.toBool() == false,
              "InstanceOf pushes false");
    }

    // 11) 用户函数：function add(a,b){return a+b;}  r = add(3,4)
    {
        std::vector<std::uint8_t> body;
        pushStr(body, "a"); op(body, 0x1C);   // GetVariable a
        pushStr(body, "b"); op(body, 0x1C);   // GetVariable b
        op(body, 0x0A);                       // Add
        op(body, 0x3E);                       // Return
        std::vector<std::uint8_t> b;
        defineFunc(b, "add", {"a", "b"}, body);   // 压函数值并自动绑定 add
        pushStr(b, "r");                     // 结果名（底）
        pushInt(b, 4); pushInt(b, 3);        // 参数（右先左后）
        pushInt(b, 2);                       // numArgs
        pushStr(b, "add");                   // funcName（顶）
        op(b, 0x3D);                         // CallFunction
        op(b, 0x1D);                         // SetVariable
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("r");
        check(it != m.globals.end() && it->second.toNumber() == 7.0, "user function add(3,4) == 7");
        check(m.globals.count("add") == 1 && m.globals["add"].type == Value::T::Function, "named function bound to 'add'");
    }
    // 12) 无返回值的函数返回 undefined
    {
        std::vector<std::uint8_t> body; op(body, 0x00);   // End（无 Return）
        std::vector<std::uint8_t> b;
        defineFunc(b, "noop", {}, body);
        pushStr(b, "z"); pushInt(b, 0); pushStr(b, "noop"); op(b, 0x3D); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("z");
        check(it != m.globals.end() && it->second.type == Value::T::Undefined, "function without return -> undefined");
    }
    // 13) 递归：fact(n) = n<2 ? 1 : n*fact(n-1);  r = fact(5) == 120
    {
        std::vector<std::uint8_t> elseB;
        pushStr(elseB, "n"); op(elseB, 0x1C);                 // n（乘法左操作数）
        pushStr(elseB, "n"); op(elseB, 0x1C); pushInt(elseB, 1); op(elseB, 0x0B);  // n-1（Subtract）
        pushInt(elseB, 1);                                    // numArgs
        pushStr(elseB, "fact");                               // funcName（顶）
        op(elseB, 0x3D);                                      // CallFunction fact(n-1)
        op(elseB, 0x0C);                                      // Multiply
        op(elseB, 0x3E);                                      // Return
        std::vector<std::uint8_t> body;
        pushStr(body, "n"); op(body, 0x1C); pushInt(body, 2); op(body, 0x0F);  // n < 2（Less）
        branchIf(body, (int)elseB.size());                    // 若 n<2 跳到 return 1
        for (auto x : elseB) body.push_back(x);
        pushInt(body, 1); op(body, 0x3E);                     // return 1
        std::vector<std::uint8_t> b;
        defineFunc(b, "fact", {"n"}, body);
        pushStr(b, "r"); pushInt(b, 5); pushInt(b, 1); pushStr(b, "fact"); op(b, 0x3D); op(b, 0x1D);
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        auto it = m.globals.find("r");
        check(it != m.globals.end() && it->second.toNumber() == 120.0, "recursive fact(5) == 120");
    }

    // 14) 非交换运算的操作数顺序（below OP top）：10-3=7, 20/4=5, 3<10=true, 10<3=false
    {
        std::vector<std::uint8_t> b;
        pushStr(b, "sub"); pushInt(b, 10); pushInt(b, 3); op(b, 0x0B); op(b, 0x1D);   // 10-3（Subtract）
        pushStr(b, "div"); pushInt(b, 20); pushInt(b, 4); op(b, 0x0D); op(b, 0x1D);   // 20/4（Divide）
        pushStr(b, "lt");  pushInt(b, 3);  pushInt(b, 10); op(b, 0x0F); op(b, 0x1D);  // 3<10（Less）
        pushStr(b, "lt2"); pushInt(b, 10); pushInt(b, 3);  op(b, 0x0F); op(b, 0x1D);  // 10<3（Less）
        Machine m; m.timeline = &tl; m.run(disassemble(b.data(), b.size()));
        check(m.globals["sub"].toNumber() == 7.0, "subtract 10-3 == 7 (operand order)");
        check(m.globals["div"].toNumber() == 5.0, "divide 20/4 == 5 (operand order)");
        check(m.globals["lt"].toBool() == true, "3 < 10 == true (operand order)");
        check(m.globals["lt2"].toBool() == false, "10 < 3 == false (operand order)");
    }

    std::printf("\nAVM1 unit test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}