// AVM1 操作码（对照 Adobe SWF 规范 / ActionScript Bytecode）
#pragma once

#include <cstdint>

namespace swf {
namespace avm1 {

enum class Op : std::uint8_t {
    End = 0x00,
    NextFrame = 0x04, PreviousFrame = 0x05, Play = 0x06, Stop = 0x07,
    ToggleQuality = 0x08, StopSounds = 0x09,
    Add = 0x0A, Subtract = 0x0B, Multiply = 0x0C, Divide = 0x0D,
    Equals = 0x0E, Less = 0x0F, And = 0x10, Or = 0x11, Not = 0x12,
    StringEquals = 0x13, StringLength = 0x14, StringExtract = 0x15,
    Pop = 0x17, ToInteger = 0x18,
    GetVariable = 0x1C, SetVariable = 0x1D, SetTarget2 = 0x20, StringAdd = 0x21,
    GetProperty = 0x22, SetProperty = 0x23, CloneSprite = 0x24,
    RemoveSprite = 0x25, Trace = 0x26, StartDrag = 0x27, EndDrag = 0x28,
    StringLess = 0x29, Throw = 0x2A, CastOp = 0x2B, ImplementsOp = 0x2C,
    RandomNumber = 0x30, MBStringLength = 0x31, CharToAscii = 0x32,
    AsciiToChar = 0x33, GetTime = 0x34, MBStringExtract = 0x35,
    MBCharToAscii = 0x36, MBAsciiToChar = 0x37, Delete = 0x3A,
    Delete2 = 0x3B, DefineLocal = 0x3C, CallFunction = 0x3D, Return = 0x3E,
    Modulo = 0x3F, NewObject = 0x40, DefineLocal2 = 0x41, InitArray = 0x42,
    InitObject = 0x43, TypeOf = 0x44, TargetPath = 0x45, Enumerate = 0x46,
    Add2 = 0x47, Less2 = 0x48, Equals2 = 0x49, ToNumber = 0x4A,
    ToString = 0x4B, PushDuplicate = 0x4C, StackSwap = 0x4D, GetMember = 0x4E,
    SetMember = 0x4F, Increment = 0x50, Decrement = 0x51, CallMethod = 0x52,
    NewMethod = 0x53, InstanceOf = 0x54, Enumerate2 = 0x55,
    BitAnd = 0x60, BitOr = 0x61, BitXor = 0x62, BitLShift = 0x63,
    BitRShift = 0x64, BitURShift = 0x65, StrictEquals = 0x66,
    Greater = 0x67, StringGreater = 0x68, Extends = 0x69,
    GotoFrame = 0x81, GetURL = 0x83, StoreRegister = 0x87, ConstantPool = 0x88,
    WaitForFrame = 0x8A, SetTarget = 0x8B, GotoLabel = 0x8C, WaitForFrame2 = 0x8D,
    DefineFunction2 = 0x8E, Try = 0x8F, With = 0x94, Push = 0x96,
    Jump = 0x99, GetURL2 = 0x9A, DefineFunction = 0x9B, If = 0x9D,
    Call = 0x9E, GotoFrame2 = 0x9F,
};

inline const char* opName(std::uint8_t code) {
    switch (code) {
        case 0x00: return "End";
        case 0x04: return "NextFrame";        case 0x05: return "PrevFrame";
        case 0x06: return "Play";             case 0x07: return "Stop";
        case 0x08: return "ToggleQuality";    case 0x09: return "StopSounds";
        case 0x0A: return "Add";              case 0x0B: return "Subtract";
        case 0x0C: return "Multiply";         case 0x0D: return "Divide";
        case 0x0E: return "Equals";           case 0x0F: return "Less";
        case 0x10: return "And";              case 0x11: return "Or";
        case 0x12: return "Not";              case 0x13: return "StringEquals";
        case 0x14: return "StringLength";     case 0x15: return "StringExtract";
        case 0x17: return "Pop";              case 0x18: return "ToInteger";
        case 0x1C: return "GetVariable";      case 0x1D: return "SetVariable";
        case 0x20: return "SetTarget2";       case 0x21: return "StringAdd";
        case 0x22: return "GetProperty";      case 0x23: return "SetProperty";
        case 0x24: return "CloneSprite";      case 0x25: return "RemoveSprite";
        case 0x26: return "Trace";            case 0x27: return "StartDrag";
        case 0x28: return "EndDrag";          case 0x29: return "StringLess";
        case 0x2A: return "Throw";            case 0x2B: return "CastOp";
        case 0x2C: return "ImplementsOp";     case 0x30: return "RandomNumber";
        case 0x31: return "MBStringLength";   case 0x32: return "CharToAscii";
        case 0x33: return "AsciiToChar";      case 0x34: return "GetTime";
        case 0x35: return "MBStringExtract";  case 0x36: return "MBCharToAscii";
        case 0x37: return "MBAsciiToChar";    case 0x3A: return "Delete";
        case 0x3B: return "Delete2";          case 0x3C: return "DefineLocal";
        case 0x3D: return "CallFunction";     case 0x3E: return "Return";
        case 0x3F: return "Modulo";           case 0x40: return "NewObject";
        case 0x41: return "DefineLocal2";     case 0x42: return "InitArray";
        case 0x43: return "InitObject";       case 0x44: return "TypeOf";
        case 0x45: return "TargetPath";       case 0x46: return "Enumerate";
        case 0x47: return "Add2";             case 0x48: return "Less2";
        case 0x49: return "Equals2";          case 0x4A: return "ToNumber";
        case 0x4B: return "ToString";         case 0x4C: return "PushDuplicate";
        case 0x4D: return "StackSwap";        case 0x4E: return "GetMember";
        case 0x4F: return "SetMember";        case 0x50: return "Increment";
        case 0x51: return "Decrement";        case 0x52: return "CallMethod";
        case 0x53: return "NewMethod";        case 0x54: return "InstanceOf";
        case 0x55: return "Enumerate2";       case 0x60: return "BitAnd";
        case 0x61: return "BitOr";            case 0x62: return "BitXor";
        case 0x63: return "BitLShift";        case 0x64: return "BitRShift";
        case 0x65: return "BitURShift";       case 0x66: return "StrictEquals";
        case 0x67: return "Greater";          case 0x68: return "StringGreater";
        case 0x69: return "Extends";          case 0x81: return "GotoFrame";
        case 0x83: return "GetURL";           case 0x87: return "StoreRegister";
        case 0x88: return "ConstantPool";     case 0x8A: return "WaitForFrame";
        case 0x8B: return "SetTarget";        case 0x8C: return "GotoLabel";
        case 0x8D: return "WaitForFrame2";    case 0x8E: return "DefineFunction2";
        case 0x8F: return "Try";              case 0x94: return "With";
        case 0x96: return "Push";             case 0x99: return "Jump";
        case 0x9A: return "GetURL2";          case 0x9B: return "DefineFunction";
        case 0x9D: return "If";               case 0x9E: return "Call";
        case 0x9F: return "GotoFrame2";
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
