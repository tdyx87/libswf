// AVM2 opcode 元数据表实现。
// 编号来源：avmplus core/opcodes.tbl（256 槽，位置即 opcode 值）；
// 操作数宽度与栈效应来源：AVM2 Overview、ruffle swf/src/avm2/read.rs。
#include "swf/Avm2Opcodes.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace swf {
namespace avm2 {
namespace {

using O = Operand;

// code, name, operand, popBase, push, popPerArg, argOperand, nameOperand, flags
#define S(c, n, o, p, q) { static_cast<std::uint8_t>(c), n, o, \
                            static_cast<std::int8_t>(p), static_cast<std::int8_t>(q), \
                            0, kNoOperand, kNoOperand, FlagStandard }
#define BR(c, n, p, q) { static_cast<std::uint8_t>(c), n, O::S24, \
                          static_cast<std::int8_t>(p), static_cast<std::int8_t>(q), \
                          0, kNoOperand, kNoOperand, FlagStandard | FlagBranch }
#define X(c, n, o, p, q, f) { static_cast<std::uint8_t>(c), n, o, \
                               static_cast<std::int8_t>(p), static_cast<std::int8_t>(q), \
                               0, kNoOperand, kNoOperand, f }
#define V(c, n, o, p, q, per, arg, nm, f) { static_cast<std::uint8_t>(c), n, o, \
                                            static_cast<std::int8_t>(p), static_cast<std::int8_t>(q), \
                                            static_cast<std::uint8_t>(per), static_cast<std::uint8_t>(arg), \
                                            static_cast<std::uint8_t>(nm), \
                                            FlagStandard | FlagVariableStack | (f) }

constexpr std::uint16_t kDebug = FlagStandard | FlagDebug;
constexpr std::uint16_t kDeprecated = FlagStandard | FlagDeprecated;
constexpr std::uint16_t kDomain = FlagStandard | FlagDomainMemory;
constexpr std::uint16_t kExt = FlagExtension;

const OpInfo kSpecs[] = {
    S(0x01, "bkpt",         O::None, 0, 0),
    S(0x02, "nop",          O::None, 0, 0),
    X(0x03, "throw",        O::None, 1, 0, FlagStandard | FlagThrow),
    V(0x04, "getsuper",     O::U30, 1, 1, 0, kNoOperand, 0, 0),
    V(0x05, "setsuper",     O::U30, 2, 0, 0, kNoOperand, 0, 0),
    S(0x06, "dxns",         O::U30, 0, 0),
    S(0x07, "dxnslate",     O::None, 1, 0),
    S(0x08, "kill",         O::U30, 0, 0),
    S(0x09, "label",        O::None, 0, 0),
    X(0x0A, "lf32x4",       O::None, 1, 1, kExt),
    X(0x0B, "sf32x4",       O::None, 2, 0, kExt),
    BR(0x0C, "ifnlt",       2, 0),
    BR(0x0D, "ifnle",       2, 0),
    BR(0x0E, "ifngt",       2, 0),
    BR(0x0F, "ifnge",       2, 0),
    BR(0x10, "jump",        0, 0),
    BR(0x11, "iftrue",      1, 0),
    BR(0x12, "iffalse",     1, 0),
    BR(0x13, "ifeq",        2, 0),
    BR(0x14, "ifne",        2, 0),
    BR(0x15, "iflt",        2, 0),
    BR(0x16, "ifle",        2, 0),
    BR(0x17, "ifgt",        2, 0),
    BR(0x18, "ifge",        2, 0),
    BR(0x19, "ifstricteq",  2, 0),
    BR(0x1A, "ifstrictne",  2, 0),
    X(0x1B, "lookupswitch", O::Switch, 1, 0, FlagStandard | FlagBranch),
    S(0x1C, "pushwith",     O::None, 1, 0),
    S(0x1D, "popscope",     O::None, 0, 0),
    S(0x1E, "nextname",     O::None, 2, 1),
    S(0x1F, "hasnext",      O::None, 2, 1),
    S(0x20, "pushnull",     O::None, 0, 1),
    S(0x21, "pushundefined", O::None, 0, 1),
    X(0x22, "pushfloat",    O::U30, 0, 1, kExt),
    S(0x23, "nextvalue",    O::None, 2, 1),
    S(0x24, "pushbyte",     O::S8, 0, 1),
    S(0x25, "pushshort",    O::U30, 0, 1),
    S(0x26, "pushtrue",     O::None, 0, 1),
    S(0x27, "pushfalse",    O::None, 0, 1),
    S(0x28, "pushnan",      O::None, 0, 1),
    S(0x29, "pop",          O::None, 1, 0),
    S(0x2A, "dup",          O::None, 1, 2),
    S(0x2B, "swap",         O::None, 2, 2),
    S(0x2C, "pushstring",   O::U30, 0, 1),
    S(0x2D, "pushint",      O::U30, 0, 1),
    S(0x2E, "pushuint",     O::U30, 0, 1),
    S(0x2F, "pushdouble",   O::U30, 0, 1),
    S(0x30, "pushscope",    O::None, 1, 0),
    S(0x31, "pushnamespace", O::U30, 0, 1),
    S(0x32, "hasnext2",     O::U30_U30, 0, 1),
    X(0x33, "pushdecimal",  O::U30, 0, 1, kExt),
    X(0x34, "pushdnan",     O::None, 0, 1, kExt),
    X(0x35, "li8",          O::None, 1, 1, kDomain),
    X(0x36, "li16",         O::None, 1, 1, kDomain),
    X(0x37, "li32",         O::None, 1, 1, kDomain),
    X(0x38, "lf32",         O::None, 1, 1, kDomain),
    X(0x39, "lf64",         O::None, 1, 1, kDomain),
    X(0x3A, "si8",          O::None, 2, 0, kDomain),
    X(0x3B, "si16",         O::None, 2, 0, kDomain),
    X(0x3C, "si32",         O::None, 2, 0, kDomain),
    X(0x3D, "sf32",         O::None, 2, 0, kDomain),
    X(0x3E, "sf64",         O::None, 2, 0, kDomain),
    S(0x40, "newfunction",  O::U30, 0, 1),
    V(0x41, "call",             O::U30, 2, 1, 1, 0, kNoOperand, 0),
    V(0x42, "construct",        O::U30, 1, 1, 1, 0, kNoOperand, 0),
    V(0x43, "callmethod",       O::U30_U30, 1, 1, 1, 1, kNoOperand, 0),
    V(0x44, "callstatic",       O::U30_U30, 1, 1, 1, 1, kNoOperand, 0),
    V(0x45, "callsuper",        O::U30_U30, 1, 1, 1, 1, 0, 0),
    V(0x46, "callproperty",     O::U30_U30, 1, 1, 1, 1, 0, 0),
    X(0x47, "returnvoid",       O::None, 0, 0, FlagStandard | FlagReturn),
    X(0x48, "returnvalue",      O::None, 1, 0, FlagStandard | FlagReturn),
    V(0x49, "constructsuper",   O::U30, 1, 0, 1, 0, kNoOperand, 0),
    V(0x4A, "constructprop",    O::U30_U30, 1, 1, 1, 1, 0, 0),
    X(0x4B, "callsuperid",      O::None, 0, 1, kExt),
    V(0x4C, "callproplex",      O::U30_U30, 1, 1, 1, 1, 0, 0),
    X(0x4D, "callinterface",    O::U30, 0, 1, kExt),
    V(0x4E, "callsupervoid",    O::U30_U30, 1, 0, 1, 1, 0, 0),
    V(0x4F, "callpropvoid",     O::U30_U30, 1, 0, 1, 1, 0, 0),
    S(0x50, "sxi1",            O::None, 1, 1),
    S(0x51, "sxi8",            O::None, 1, 1),
    S(0x52, "sxi16",           O::None, 1, 1),
    V(0x53, "applytype",        O::U30, 1, 1, 1, 0, kNoOperand, 0),
    X(0x54, "pushfloat4",       O::U30, 0, 1, kExt),
    V(0x55, "newobject",        O::U30, 0, 1, 2, 0, kNoOperand, 0),
    V(0x56, "newarray",         O::U30, 0, 1, 1, 0, kNoOperand, 0),
    S(0x57, "newactivation",    O::None, 0, 1),
    S(0x58, "newclass",         O::U30, 1, 1),
    V(0x59, "getdescendants",   O::U30, 1, 1, 0, kNoOperand, 0, 0),
    S(0x5A, "newcatch",         O::U30, 0, 1),
    X(0x5B, "findpropglobalstrict", O::U30, 0, 1, kExt),
    X(0x5C, "findpropglobal",   O::U30, 0, 1, kExt),
    V(0x5D, "findpropstrict",   O::U30, 0, 1, 0, kNoOperand, 0, 0),
    V(0x5E, "findproperty",     O::U30, 0, 1, 0, kNoOperand, 0, 0),
    V(0x5F, "finddef",          O::U30, 0, 1, 0, kNoOperand, 0, 0),
    V(0x60, "getlex",           O::U30, 0, 1, 0, kNoOperand, 0, 0),
    V(0x61, "setproperty",      O::U30, 2, 0, 0, kNoOperand, 0, 0),
    S(0x62, "getlocal",         O::U30, 0, 1),
    S(0x63, "setlocal",         O::U30, 1, 0),
    S(0x64, "getglobalscope",   O::None, 0, 1),
    S(0x65, "getscopeobject",   O::U8, 0, 1),
    V(0x66, "getproperty",      O::U30, 1, 1, 0, kNoOperand, 0, 0),
    S(0x67, "getouterscope",    O::U30, 0, 1),
    V(0x68, "initproperty",     O::U30, 2, 0, 0, kNoOperand, 0, 0),
    // 0x69 / 0x6B 在 avmplus 中是 ABC_UNUSED_OP，保留槽位，不定义名字。
    V(0x6A, "deleteproperty",   O::U30, 1, 1, 0, kNoOperand, 0, 0),
    S(0x6C, "getslot",          O::U30, 1, 1),
    S(0x6D, "setslot",          O::U30, 2, 0),
    S(0x6E, "getglobalslot",    O::U30, 0, 1),
    S(0x6F, "setglobalslot",    O::U30, 1, 0),
    S(0x70, "convert_s",        O::None, 1, 1),
    S(0x71, "esc_xelem",        O::None, 1, 1),
    S(0x72, "esc_xattr",        O::None, 1, 1),
    S(0x73, "convert_i",        O::None, 1, 1),
    S(0x74, "convert_u",        O::None, 1, 1),
    S(0x75, "convert_d",        O::None, 1, 1),
    S(0x76, "convert_b",        O::None, 1, 1),
    S(0x77, "convert_o",        O::None, 1, 1),
    S(0x78, "checkfilter",      O::None, 1, 1),
    X(0x79, "convert_f",        O::None, 1, 1, kExt),
    X(0x7A, "unplus",           O::None, 1, 1, kExt),
    X(0x7B, "convert_f4",       O::None, 1, 1, kExt),
    V(0x80, "coerce",           O::U30, 1, 1, 0, kNoOperand, 0, 0),
    X(0x81, "coerce_b",         O::None, 1, 1, kDeprecated),
    S(0x82, "coerce_a",         O::None, 1, 1),
    X(0x83, "coerce_i",         O::None, 1, 1, kDeprecated),
    X(0x84, "coerce_d",         O::None, 1, 1, kDeprecated),
    S(0x85, "coerce_s",         O::None, 1, 1),
    V(0x86, "astype",           O::U30, 1, 1, 0, kNoOperand, 0, 0),
    S(0x87, "astypelate",       O::None, 2, 1),
    X(0x88, "coerce_u",         O::None, 1, 1, kDeprecated),
    S(0x89, "coerce_o",         O::None, 1, 1),
    S(0x90, "negate",           O::None, 1, 1),
    S(0x91, "increment",        O::None, 1, 1),
    S(0x92, "inclocal",         O::U30, 0, 0),
    S(0x93, "decrement",        O::None, 1, 1),
    S(0x94, "declocal",         O::U30, 0, 0),
    S(0x95, "typeof",           O::None, 1, 1),
    S(0x96, "not",              O::None, 1, 1),
    S(0x97, "bitnot",           O::None, 1, 1),
    S(0xA0, "add",              O::None, 2, 1),
    S(0xA1, "subtract",         O::None, 2, 1),
    S(0xA2, "multiply",         O::None, 2, 1),
    S(0xA3, "divide",           O::None, 2, 1),
    S(0xA4, "modulo",           O::None, 2, 1),
    S(0xA5, "lshift",           O::None, 2, 1),
    S(0xA6, "rshift",           O::None, 2, 1),
    S(0xA7, "urshift",          O::None, 2, 1),
    S(0xA8, "bitand",           O::None, 2, 1),
    S(0xA9, "bitor",            O::None, 2, 1),
    S(0xAA, "bitxor",           O::None, 2, 1),
    S(0xAB, "equals",           O::None, 2, 1),
    S(0xAC, "strictequals",     O::None, 2, 1),
    S(0xAD, "lessthan",         O::None, 2, 1),
    S(0xAE, "lessequals",       O::None, 2, 1),
    S(0xAF, "greaterthan",      O::None, 2, 1),
    S(0xB0, "greaterequals",    O::None, 2, 1),
    S(0xB1, "instanceof",       O::None, 2, 1),
    V(0xB2, "istype",           O::U30, 1, 1, 0, kNoOperand, 0, 0),
    S(0xB3, "istypelate",       O::None, 2, 1),
    S(0xB4, "in",               O::None, 2, 1),
    S(0xC0, "increment_i",      O::None, 1, 1),
    S(0xC1, "decrement_i",      O::None, 1, 1),
    S(0xC2, "inclocal_i",       O::U30, 0, 0),
    S(0xC3, "declocal_i",       O::U30, 0, 0),
    S(0xC4, "negate_i",         O::None, 1, 1),
    S(0xC5, "add_i",            O::None, 2, 1),
    S(0xC6, "subtract_i",       O::None, 2, 1),
    S(0xC7, "multiply_i",       O::None, 2, 1),
    S(0xD0, "getlocal0",        O::None, 0, 1),
    S(0xD1, "getlocal1",        O::None, 0, 1),
    S(0xD2, "getlocal2",        O::None, 0, 1),
    S(0xD3, "getlocal3",        O::None, 0, 1),
    S(0xD4, "setlocal0",        O::None, 1, 0),
    S(0xD5, "setlocal1",        O::None, 1, 0),
    S(0xD6, "setlocal2",        O::None, 1, 0),
    S(0xD7, "setlocal3",        O::None, 1, 0),
    X(0xEE, "abs_jump",         O::None, 0, 0, kExt),
    X(0xEF, "debug",            O::Debug, 0, 0, kDebug),
    X(0xF0, "debugline",        O::U30, 0, 0, kDebug),
    X(0xF1, "debugfile",        O::U30, 0, 0, kDebug),
    X(0xF2, "bkptline",         O::U30, 0, 0, kDebug),
    S(0xF3, "timestamp",        O::None, 0, 0),
    X(0xF4, "restargc",         O::None, 0, 0, kExt),
    X(0xF5, "restarg",          O::None, 0, 1, kExt),
};

#undef S
#undef BR
#undef X
#undef V

const std::array<OpInfo, 256>& table() {
    static const std::array<OpInfo, 256> t = [] {
        std::array<OpInfo, 256> a{};
        for (const auto& s : kSpecs) a[s.code] = s;
        return a;
    }();
    return t;
}

}  // namespace

const OpInfo& opInfo(std::uint8_t code) { return table()[code]; }

const char* opName(std::uint8_t code) {
    const char* n = table()[code].name;
    return n ? n : "reserved";
}

const char* operandName(Operand o) {
    switch (o) {
        case Operand::None:    return "-";
        case Operand::U8:      return "u8";
        case Operand::S8:      return "s8";
        case Operand::U30:     return "u30";
        case Operand::S24:     return "s24";
        case Operand::U30_U30: return "u30,u30";
        case Operand::Debug:   return "u8,u30,u8,u30";
        case Operand::Switch:  return "s24,u30,s24[]";
    }
    return "?";
}

std::vector<std::uint8_t> standardOpcodes() {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 256; ++i)
        if (table()[i].standard()) out.push_back(static_cast<std::uint8_t>(i));
    return out;
}

std::size_t standardOpcodeCount() { return standardOpcodes().size(); }

Instruction decode(const std::uint8_t* code, std::size_t size, std::size_t pc) {
    Instruction ins;
    ins.pc = pc;
    if (!code || pc >= size) {
        ins.truncated = true;
        ins.size = 0;
        return ins;
    }
    ins.code = code[pc];
    ins.info = &opInfo(ins.code);
    std::size_t i = pc + 1;

    auto readU8 = [&](std::int64_t& out) -> bool {
        if (i >= size) return false;
        out = code[i++];
        return true;
    };
    auto readU30 = [&](std::int64_t& out) -> bool {
        std::uint32_t r = 0;
        for (int k = 0; k < 5; ++k) {
            if (i >= size) return false;
            const std::uint8_t b = code[i++];
            if (k == 4) { r |= static_cast<std::uint32_t>(b & 0x0F) << 28; break; }
            r |= static_cast<std::uint32_t>(b & 0x7F) << (7 * k);
            if (!(b & 0x80)) break;
        }
        out = r;
        return true;
    };
    auto readS24 = [&](std::int64_t& out) -> bool {
        if (i + 3 > size) return false;
        std::uint32_t v = static_cast<std::uint32_t>(code[i]) |
                          (static_cast<std::uint32_t>(code[i + 1]) << 8) |
                          (static_cast<std::uint32_t>(code[i + 2]) << 16);
        i += 3;
        if (v & 0x800000u) v |= 0xFF000000u;
        out = static_cast<std::int32_t>(v);
        return true;
    };

    bool ok = true;
    std::int64_t v = 0;
    switch (ins.info->operand) {
        case Operand::None: break;
        case Operand::U8:
        case Operand::S8:
            ok = readU8(v);
            if (ok) ins.operands.push_back(v);
            break;
        case Operand::U30:
            ok = readU30(v);
            if (ok) ins.operands.push_back(v);
            break;
        case Operand::S24:
            ok = readS24(v);
            if (ok) ins.operands.push_back(v);
            break;
        case Operand::U30_U30:
            for (int k = 0; k < 2 && ok; ++k) {
                ok = readU30(v);
                if (ok) ins.operands.push_back(v);
            }
            break;
        case Operand::Debug:
            for (int k = 0; k < 4 && ok; ++k) {
                ok = (k == 0 || k == 2) ? readU8(v) : readU30(v);
                if (ok) ins.operands.push_back(v);
            }
            break;
        case Operand::Switch: {
            ok = readS24(v);
            if (ok) ins.operands.push_back(v);   // default offset
            std::int64_t caseCount = 0;
            if (ok) ok = readU30(caseCount);
            if (ok) ins.operands.push_back(caseCount);
            // (caseCount+1) 个 s24；畸形字节码会在这里因 EOF 立即停止，不会放大分配。
            if (ok && caseCount >= 0 && caseCount <= 0x0FFFFFFF) {
                for (std::int64_t k = 0; k <= caseCount && ok; ++k) {
                    ok = readS24(v);
                    if (ok) ins.operands.push_back(v);
                }
            }
            break;
        }
    }
    ins.size = ok ? (i - pc) : (size - pc);
    ins.truncated = !ok;
    return ins;
}

void stackEffect(const Instruction& ins, int runtimeNamePops, int& pop, int& push) {
    const OpInfo& info = ins.info ? *ins.info : opInfo(ins.code);
    pop = info.popBase;
    push = info.push;
    if (info.argOperand != kNoOperand && info.argOperand < ins.operands.size()) {
        std::int64_t argc = ins.operands[info.argOperand];
        if (argc < 0) argc = 0;
        if (argc > 65535) argc = 65535;   // 畸形字节码保护
        pop += static_cast<int>(argc) * static_cast<int>(info.popPerArg);
    }
    if (info.nameOperand != kNoOperand && runtimeNamePops > 0) pop += runtimeNamePops;
    if (pop < 0) pop = 0;
}

std::string disassemble(const std::uint8_t* code, std::size_t size) {
    std::string out;
    char buf[256];
    for (std::size_t pc = 0; pc < size;) {
        const Instruction ins = decode(code, size, pc);
        if (ins.size == 0) break;
        const char* name = ins.info && ins.info->valid() ? ins.info->name : nullptr;
        if (name) {
            std::snprintf(buf, sizeof(buf), "%05zu  0x%02X  %-22s", pc, ins.code, name);
        } else {
            std::snprintf(buf, sizeof(buf), "%05zu  0x%02X  %-22s", pc, ins.code, "db");
        }
        out += buf;
        if (!name) {
            std::snprintf(buf, sizeof(buf), " 0x%02X", ins.code);
            out += buf;
        }
        for (std::size_t k = 0; k < ins.operands.size(); ++k) {
            std::snprintf(buf, sizeof(buf), " %lld", static_cast<long long>(ins.operands[k]));
            out += buf;
        }
        if (ins.truncated) out += "  <truncated>";
        out += "\n";
        pc += ins.size;
    }
    return out;
}

}  // namespace avm2
}  // namespace swf
