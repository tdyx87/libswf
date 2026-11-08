// AVM2（AS3 / ABC 字节码）opcode 元数据表。
//
// 依据 AVM2 Overview 与 avmplus core/opcodes.tbl 的 256 槽编号，并用
// ruffle swf/src/avm2/read.rs 的操作数读取代码核对字节宽度。
// 该表是解释器的**单一事实来源**：编号、操作数布局、栈效应、控制流属性、
// 反汇编以及「未知 opcode 安全跳过」都从这里派生，避免各处手写偏移错位。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swf {
namespace avm2 {

// 操作数编码：决定一条指令在字节流里占用多少字节。
enum class Operand : std::uint8_t {
    None = 0,
    U8,        // 1 字节无符号（getscopeobject）
    S8,        // 1 字节（pushbyte 的值按 s8 解释）
    U30,       // 变长 u30，1..5 字节
    S24,       // 3 字节有符号跳转偏移
    U30_U30,   // 两个变长 u30（multiname + argCount）
    Debug,     // u8 + u30 + u8 + u30（0xEF debug）
    Switch,    // s24 default + u30 caseCount + (caseCount+1)*s24
};

enum : std::uint16_t {
    FlagStandard      = 1u << 0,  // 标准 Flash Player 指令（覆盖度统计口径）
    FlagBranch        = 1u << 1,
    FlagReturn        = 1u << 2,
    FlagThrow         = 1u << 3,
    FlagDebug         = 1u << 4,
    FlagDeprecated    = 1u << 5,
    FlagDomainMemory  = 1u << 6,  // Alchemy / domainMemory 扩展（已正式支持）
    FlagVariableStack = 1u << 7,  // 栈效应依赖 argCount 或运行时 multiname
    FlagExtension     = 1u << 8,  // 非标准 / 内部 / 未发布指令
};

constexpr std::uint8_t kNoOperand = 0xFF;

struct OpInfo {
    std::uint8_t code = 0;
    const char* name = nullptr;             // nullptr = 保留 / 非法 opcode
    Operand operand = Operand::None;
    std::int8_t popBase = 0;                // 不含 argCount 与运行时 multiname 的弹栈数
    std::int8_t push = 0;
    std::uint8_t popPerArg = 0;             // 每个实参弹出的栈值数
    std::uint8_t argOperand = kNoOperand;   // operands[] 中 argCount 的下标
    std::uint8_t nameOperand = kNoOperand;  // operands[] 中 multiname 索引的下标
    std::uint16_t flags = 0;

    bool valid() const { return name != nullptr; }
    bool standard() const { return valid() && (flags & FlagStandard) != 0; }
    bool variableStack() const { return (flags & FlagVariableStack) != 0; }
};

// 256 槽表；未定义槽位返回 name == nullptr 的静态条目。
const OpInfo& opInfo(std::uint8_t code);
const char* opName(std::uint8_t code);
const char* operandName(Operand o);

// 所有标准 opcode（升序），用于解释器覆盖度检查。
std::vector<std::uint8_t> standardOpcodes();
std::size_t standardOpcodeCount();

struct Instruction {
    std::size_t pc = 0;
    std::uint8_t code = 0;
    const OpInfo* info = nullptr;
    // u8/u30 为非负值；s24 为符号扩展后的有符号值。
    std::vector<std::int64_t> operands;
    std::size_t size = 1;    // 含 opcode 字节的总长度
    bool truncated = false;  // 操作数被截断（字节码损坏）
};

// 从 code[pc]（pc 指向 opcode 字节）解码一条指令，不越界、不抛异常。
Instruction decode(const std::uint8_t* code, std::size_t size, std::size_t pc);

// 计算真实栈效应。runtimeNamePops 由调用方按 ABC multiname 种类给出（0/1/2）。
void stackEffect(const Instruction& ins, int runtimeNamePops, int& pop, int& push);

// 把一段 method_body.code 反汇编成可读文本（未知槽位输出 db 0xNN）。
std::string disassemble(const std::uint8_t* code, std::size_t size);

}  // namespace avm2
}  // namespace swf
