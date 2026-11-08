// AVM2 opcode 元数据 / 操作数解码 / 栈效应 / 解释器覆盖度单元测试。
// CTest: swfparser_avm2_vm。
#include "swf/Avm2Opcodes.hpp"
#include "swf/Abc.hpp"
#include "swf/Fvm.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace a = swf::avm2;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static bool nameIs(std::uint8_t op, const char* n) {
    return a::opInfo(op).valid() && std::strcmp(a::opInfo(op).name, n) == 0;
}

int main() {
    std::printf("AVM2 opcode metadata tests\n");

    // ---- 1. 编号核对（对齐 avmplus opcodes.tbl / AVM2 Overview）----
    check(nameIs(0x01, "bkpt") && nameIs(0x02, "nop") && nameIs(0x03, "throw"),
          "0x01..0x03 bkpt/nop/throw");
    check(nameIs(0x10, "jump") && nameIs(0x1B, "lookupswitch") &&
          nameIs(0x24, "pushbyte") && nameIs(0x25, "pushshort"),
          "0x10/0x1B/0x24/0x25 jump/lookupswitch/pushbyte/pushshort");
    check(nameIs(0x32, "hasnext2") && nameIs(0x46, "callproperty") &&
          nameIs(0x49, "constructsuper") && nameIs(0x4F, "callpropvoid"),
          "0x32/0x46/0x49/0x4F hasnext2/callproperty/constructsuper/callpropvoid");
    check(nameIs(0x58, "newclass") && nameIs(0x65, "getscopeobject") &&
          nameIs(0x67, "getouterscope") && nameIs(0x6E, "getglobalslot"),
          "0x58/0x65/0x67/0x6E newclass/getscopeobject/getouterscope/getglobalslot");
    check(nameIs(0x80, "coerce") && nameIs(0x86, "astype") &&
          nameIs(0xB1, "instanceof") && nameIs(0xB4, "in"),
          "0x80/0x86/0xB1/0xB4 coerce/astype/instanceof/in");
    check(nameIs(0xC5, "add_i") && nameIs(0xD0, "getlocal0"),
          "0xC5/0xD0 add_i/getlocal0");
    check(nameIs(0xEF, "debug") && nameIs(0xF0, "debugline") &&
          nameIs(0xF1, "debugfile") && nameIs(0xF2, "bkptline") &&
          nameIs(0xF3, "timestamp"),
          "0xEF..0xF3 debug/debugline/debugfile/bkptline/timestamp");
    check(!a::opInfo(0x00).valid() && !a::opInfo(0x3F).valid() &&
          !a::opInfo(0x69).valid() && !a::opInfo(0x6B).valid() &&
          !a::opInfo(0x8A).valid() && !a::opInfo(0x98).valid() &&
          !a::opInfo(0xB5).valid() && !a::opInfo(0xC8).valid() &&
          !a::opInfo(0xD8).valid() && !a::opInfo(0xFE).valid(),
          "reserved opcode slots are invalid");

    // ---- 2. 操作数布局 ----
    check(a::opInfo(0x65).operand == a::Operand::U8 &&
          a::opInfo(0x32).operand == a::Operand::U30_U30 &&
          a::opInfo(0xEF).operand == a::Operand::Debug &&
          a::opInfo(0x1B).operand == a::Operand::Switch &&
          a::opInfo(0x11).operand == a::Operand::S24,
          "operand layouts u8/u30x2/debug/switch/s24");
    check(a::opInfo(0x10).flags & a::FlagBranch, "jump is a branch");
    check(a::opInfo(0x47).flags & a::FlagReturn, "returnvoid is a return");
    check(a::opInfo(0x03).flags & a::FlagThrow, "throw can throw");

    // ---- 3. 操作数解码（字节边界）----
    {
        const std::uint8_t code[] = {0x2C, 0x81, 0x01};   // pushstring u30=129
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 3 && ins.operands.size() == 1 && ins.operands[0] == 129,
              "u30 multi-byte decoding (129)");
    }
    {
        const std::uint8_t code[] = {0x10, 0x2F, 0xD0, 0xFE};  // jump -77777
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 4 && ins.operands.size() == 1 && ins.operands[0] == -77777,
              "s24 signed decoding (-77777)");
    }
    {
        const std::uint8_t code[] = {0xEF, 0x01, 0x05, 0x02, 0x80, 0x01};
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 6 && ins.operands.size() == 4 &&
              ins.operands[0] == 1 && ins.operands[1] == 5 &&
              ins.operands[2] == 2 && ins.operands[3] == 128,
              "debug operands u8,u30,u8,u30");
    }
    {
        // default=0, caseCount=1, case0=2, case1=-2
        const std::uint8_t code[] = {0x1B, 0x00,0x00,0x00, 0x01,
                                     0x02,0x00,0x00, 0xFE,0xFF,0xFF};
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 11 && ins.operands.size() == 4 &&
              ins.operands[0] == 0 && ins.operands[1] == 1 &&
              ins.operands[2] == 2 && ins.operands[3] == -2,
              "lookupswitch variable-length operands");
    }
    {
        const std::uint8_t code[] = {0x46, 0x0A, 0x03};   // callproperty mn=10 argc=3
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 3 && ins.operands.size() == 2 &&
              ins.operands[0] == 10 && ins.operands[1] == 3,
              "callproperty two u30 operands");
    }
    {
        const std::uint8_t code[] = {0x65, 0x07};          // getscopeobject u8=7
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.size == 2 && ins.operands.size() == 1 && ins.operands[0] == 7,
              "getscopeobject u8 operand");
    }
    {
        const std::uint8_t code[] = {0x00, 0x24, 0x07};   // reserved + pushbyte 7
        const a::Instruction r = a::decode(code, sizeof(code), 0);
        const a::Instruction p = a::decode(code, sizeof(code), 1);
        check(r.size == 1 && !r.info->valid() && p.size == 2 && p.operands[0] == 7,
              "reserved opcode consumes exactly one byte");
    }
    {
        const std::uint8_t code[] = {0x2C};                 // 截断的 u30
        const a::Instruction ins = a::decode(code, sizeof(code), 0);
        check(ins.truncated && ins.size == 1, "truncated operand is reported");
    }

    // ---- 4. 栈效应 ----
    {
        auto effect = [](std::uint8_t op, std::vector<std::int64_t> ops, int rt,
                         int& pop, int& push) {
            a::Instruction ins;
            ins.code = op;
            ins.info = &a::opInfo(op);
            ins.operands = std::move(ops);
            a::stackEffect(ins, rt, pop, push);
        };
        int pop = 0, push = 0;
        effect(0xA0, {}, 0, pop, push);
        check(pop == 2 && push == 1, "add pops 2 pushes 1");
        effect(0x2A, {}, 0, pop, push);
        check(pop == 1 && push == 2, "dup pops 1 pushes 2");
        effect(0x46, {10, 3}, 0, pop, push);
        check(pop == 4 && push == 1, "callproperty argc=3 pops 1+3");
        effect(0x46, {10, 3}, 2, pop, push);
        check(pop == 6 && push == 1, "callproperty runtime multiname adds 2 pops");
        effect(0x55, {3}, 0, pop, push);
        check(pop == 6 && push == 1, "newobject pops 2 per key/value pair");
        effect(0x66, {5}, 1, pop, push);
        check(pop == 2 && push == 1, "getproperty with runtime name pops 2");
        effect(0x10, {0}, 0, pop, push);
        check(pop == 0 && push == 0, "jump has no static stack effect");
        effect(0x61, {5}, 0, pop, push);
        check(pop == 2 && push == 0, "setproperty pops obj+value");
    }

    // ---- 5. 反汇编 ----
    {
        const std::uint8_t code[] = {0x24, 0x07, 0x4F, 0x05, 0x00, 0x47};
        const std::string text = a::disassemble(code, sizeof(code));
        check(text.find("pushbyte") != std::string::npos &&
              text.find("callpropvoid") != std::string::npos &&
              text.find("returnvoid") != std::string::npos,
              "disassembler prints mnemonics and keeps instruction boundaries");
        check(text.find("00005") != std::string::npos,
              "disassembler reports correct pc for third instruction");
    }

    // ---- 6. 解释器覆盖度：标准 opcode 必须全部有处理器，保留槽不许注册 ----
    bool impl[256] = {};
    for (std::uint8_t op : swf::fvm::implementedOpcodes()) impl[op] = true;
    const auto standard = a::standardOpcodes();
    check(standard.size() >= 140, "standard opcode table is complete");
    std::size_t missing = 0, bad = 0;
    for (std::uint8_t op : standard) {
        if (!impl[op]) {
            if (missing < 12)
                std::printf("       missing 0x%02X %s\n", op, a::opName(op));
            ++missing;
        }
    }
    for (int i = 0; i < 256; ++i)
        if (impl[i] && !a::opInfo(static_cast<std::uint8_t>(i)).valid()) ++bad;
    check(missing == 0, "every standard AVM2 opcode has an interpreter handler");
    check(bad == 0, "no handler registered on a reserved opcode slot");
    check(impl[0xB4] && impl[0x81] && impl[0x88] && impl[0x35] && impl[0x3E],
          "previously missing in/coerce/domainMemory opcodes are implemented");

    // ---- 7. ABC 常量池：RTQName / TypeName / 未知 kind ----
    {
        // 最小 ABC：无方法/实例/脚本/方法体，常量池含 3 个 multiname。
        std::vector<std::uint8_t> bytes = {
            0x10,0x00, 0x2E,0x00,             // minor=16 major=46
            0x01,                             // int_count
            0x01,                             // uint_count
            0x01,                             // double_count
            0x02, 0x03,'f','o','o',          // string_count=2, "foo"
            0x01,                             // namespace_count
            0x01,                             // ns_set_count
            0x04,                             // multiname_count=4
            0x07, 0x00,0x01,                 // [1] QName ns=0 name=1
            0x0F, 0x01,                      // [2] RTQName name=1（ns 由栈提供）
            0x1D, 0x01, 0x02, 0x01,0x01,     // [3] TypeName name=1 paramCount=2
            0x00,                             // method_count
            0x00,                             // metadata_count
            0x00,                             // instance_count
            0x00,                             // script_count
            0x00,                             // method_body_count
        };
        swf::abc::AbcFile f;
        std::string err;
        check(swf::abc::parseAbc(bytes.data(), bytes.size(), f, err),
              "minimal ABC file parses");
        check(f.str(1) == "foo", "ABC string constant pool entry");
        const swf::abc::Multiname* rt = f.mn(2);
        check(rt && rt->kind == swf::abc::Multiname::Kind::RTQName && rt->name == 1 &&
              f.str(rt->name) == "foo",
              "RTQName keeps its constant-pool name");
        const swf::abc::Multiname* tn = f.mn(3);
        check(tn && tn->kind == swf::abc::Multiname::Kind::TypeName &&
              tn->typeArgs.size() == 2,
              "TypeName param_count is u30 and type args are parsed");
        auto bad = bytes;
        bad[18] = 0x7F;                       // 把 RTQName 改成未知 multiname kind
        swf::abc::AbcFile f2;
        std::string err2;
        check(!swf::abc::parseAbc(bad.data(), bad.size(), f2, err2),
              "unknown multiname kind aborts ABC parsing");
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
