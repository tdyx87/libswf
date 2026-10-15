// 对照逆向结果：sub_4998E0（AVM1 / ActionScript 2 解释器，.as.doactions）
// switch 共 85 个不同 case，值域 0x00–0x69；操作数栈 this+424，栈指针 this+452
#pragma once

#include <cstdint>

namespace swf {
namespace avm1 {

enum class Op : std::uint8_t {
    End = 0x00, NextFrame = 0x01, PreviousFrame = 0x02, Play = 0x03, Stop = 0x04,
    ToggleQuality = 0x05, StopSounds = 0x06, Add = 0x07, Subtract = 0x08,
    Multiply = 0x09, Divide = 0x0A, Equals = 0x0B, Less = 0x0C, And = 0x0D,
    Or = 0x0E, Not = 0x0F, StringEquals = 0x10, StringLength = 0x11,
    StringExtract = 0x12, ToInteger0 = 0x13, ToInteger = 0x14,
    MbStringLength = 0x15, Pop = 0x17, ToNumber = 0x18,
    GetVariable = 0x1C, SetVariable = 0x1D, SetTarget2 = 0x20, StringAdd = 0x21,
    GetProperty = 0x22, SetProperty = 0x23, CloneSprite = 0x24,
    RemoveSprite = 0x25, Trace = 0x26, StartDrag = 0x27, EndDrag = 0x28,
    StringLess = 0x29, Throw = 0x2A, Cast = 0x2B, Implements = 0x2C,
    RandomNumber = 0x30, MbStringLength2 = 0x31, CharToAscii = 0x32,
    AsciiToChar = 0x33, GetTime = 0x34, MbStringExtract = 0x35,
    MbCharToAscii = 0x36, MbAsciiToChar = 0x37, Delete = 0x3A,
    Delete2 = 0x3B, DefineLocal = 0x3C, CallFunction = 0x3D, Return = 0x3E,
    Modulo = 0x3F, Add2 = 0x40, Less2 = 0x41, Equals2 = 0x42,
    ToNumber2 = 0x43, ToString = 0x44, PushDuplicate = 0x45, StackSwap = 0x46,
    GetMember = 0x47, SetMember = 0x48, Increment = 0x49, Decrement = 0x4A,
    CallMethod = 0x4B, NewMethod = 0x4C, InstanceOf = 0x4D,
    Enumerate2 = 0x4E, BitAnd0 = 0x50, BitOr0 = 0x51, BitXor0 = 0x52,
    BitLShift0 = 0x53, BitRShift0 = 0x54, BitURShift0 = 0x55,
    BitAnd = 0x60, BitOr = 0x61, BitXor = 0x62, BitLShift = 0x63,
    BitRShift = 0x64, BitURShift = 0x65, StrictEquals = 0x66,
    Greater = 0x67, StringGreater = 0x68, GreaterEquals = 0x69,
};

inline const char* opName(std::uint8_t code) {
    switch (code) {
        case 0x00: return "End";              case 0x01: return "NextFrame";
        case 0x02: return "PreviousFrame";    case 0x03: return "Play";
        case 0x04: return "Stop";             case 0x05: return "ToggleQuality";
        case 0x06: return "StopSounds";       case 0x07: return "Add";
        case 0x08: return "Subtract";         case 0x09: return "Multiply";
        case 0x0A: return "Divide";           case 0x0B: return "Equals";
        case 0x0C: return "Less";             case 0x0D: return "And";
        case 0x0E: return "Or";               case 0x0F: return "Not";
        case 0x10: return "StringEquals";     case 0x11: return "StringLength";
        case 0x12: return "StringExtract";    case 0x13: return "ToInteger";
        case 0x14: return "ToInteger";        case 0x15: return "MBStringLength";
        case 0x17: return "Pop";              case 0x18: return "ToNumber";
        case 0x1C: return "GetVariable";      case 0x1D: return "SetVariable";
        case 0x20: return "SetTarget2";       case 0x21: return "StringAdd";
        case 0x22: return "GetProperty";      case 0x23: return "SetProperty";
        case 0x24: return "CloneSprite";      case 0x25: return "RemoveSprite";
        case 0x26: return "Trace";            case 0x27: return "StartDrag";
        case 0x28: return "EndDrag";          case 0x29: return "StringLess";
        case 0x2A: return "Throw";            case 0x2B: return "Cast";
        case 0x2C: return "Implements";       case 0x30: return "RandomNumber";
        case 0x31: return "MBStringLength";   case 0x32: return "CharToAscii";
        case 0x33: return "AsciiToChar";      case 0x34: return "GetTime";
        case 0x35: return "MBStringExtract";  case 0x36: return "MBCharToAscii";
        case 0x37: return "MBAsciiToChar";    case 0x3A: return "Delete";
        case 0x3B: return "Delete2";          case 0x3C: return "DefineLocal";
        case 0x3D: return "CallFunction";     case 0x3E: return "Return";
        case 0x3F: return "Modulo";           case 0x40: return "Add2";
        case 0x41: return "Less2";            case 0x42: return "Equals2";
        case 0x43: return "ToNumber";         case 0x44: return "ToString";
        case 0x45: return "PushDuplicate";    case 0x46: return "StackSwap";
        case 0x47: return "GetMember";        case 0x48: return "SetMember";
        case 0x49: return "Increment";        case 0x4A: return "Decrement";
        case 0x4B: return "CallMethod";       case 0x4C: return "NewMethod";
        case 0x4D: return "InstanceOf";       case 0x4E: return "Enumerate2";
        case 0x60: return "BitAnd";           case 0x61: return "BitOr";
        case 0x62: return "BitXor";           case 0x63: return "BitLShift";
        case 0x64: return "BitRShift";        case 0x65: return "BitURShift";
        case 0x66: return "StrictEquals";     case 0x67: return "Greater";
        case 0x68: return "StringGreater";    case 0x69: return "GreaterEquals";
        default:   return "Unknown";
    }
}

// 逆向确认的实现归属（handler 名对应 sub_4998E0 中的被调）
struct OpImpl {
    std::uint8_t code;
    const char* handler;  // 0 表示内联实现
};

inline const OpImpl* opImplTable() {
    static const OpImpl table[] = {
        {0x17, "sub_4964EB"},   // Pop        -> 操作数栈 this+424
        {0x1C, "sub_496716"},   // GetVariable
        {0x1D, "sub_496716"},   // SetVariable
        {0x20, "sub_498F40"},   // SetTarget2
        {0x22, "sub_497DD0"},   // GetProperty
        {0x23, "sub_497ED0"},   // SetProperty
        {0x24, "sub_497FD0"},   // CloneSprite
        {0x25, "sub_498120"},   // RemoveSprite
        {0x26, "sub_497B10"},   // Trace
        {0x27, "sub_4981F0"},   // StartDrag
        {0x28, "sub_4361B0"},   // EndDrag
        {0x2A, "sub_4994F0"},   // Throw
        {0x2B, "sub_496D10"},   // Cast
        {0x2C, "sub_496B60"},   // Implements
        {0x34, "COERCE_FLOAT"}, // GetTime    -> 返回毫秒 float
        {0x3A, "sub_498410"},   // Delete     (以 v69 == 58 区分)
        {0x3B, "sub_498410"},   // Delete2
        {0x45, "sub_496840"},   // PushDuplicate
        {0x08, "sub_4223D0"},   // ToggleQuality
    };
    return table;
}

inline constexpr int opImplCount() { return 19; }

}  // namespace avm1
}  // namespace swf
