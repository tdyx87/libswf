// 对照逆向结果：sub_48BA50 的标签分派器
// 两个 switch：
//   A 组 = 定义/资源类标签（立即处理）
//   B 组 = 帧/执行类标签（条件 !(movie+624 & 0x800) && !v18 时处理）
#pragma once

#include <cstdint>

namespace swf {

enum class TagGroup : std::uint8_t { Definition, Execution, Ignored };

enum class TagCode : std::uint16_t {
    End = 0,
    ShowFrame = 1,
    DefineShape = 2,
    FreeCharacter = 3,
    PlaceObject = 4,
    RemoveObject = 5,
    DefineBits = 6,
    DefineButton = 7,
    JPEGTables = 8,
    SetBackgroundColor = 9,
    DefineFont = 10,
    DefineText = 11,
    DoAction = 12,
    DefineFontInfo = 13,
    DefineSound = 14,
    StartSound = 15,
    DefineButtonSound = 17,
    SoundStreamHead = 18,
    SoundStreamBlock = 19,
    DefineBitsLossless = 20,
    DefineBitsJPEG2 = 21,
    DefineShape2 = 22,
    DefineButtonCxform = 23,
    PlaceObject2 = 26,
    RemoveObject2 = 28,
    DefineShape3 = 32,
    DefineText2 = 33,
    DefineButton2 = 34,
    DefineBitsJPEG3 = 35,
    DefineBitsLossless2 = 36,
    DefineEditText = 37,
    DefineSprite = 39,
    FrameLabel = 43,
    SoundStreamHead2 = 45,
    DefineMorphShape = 46,
    DefineFont2 = 48,
    ExportAssets = 56,
    ImportAssets = 57,
    DoInitAction = 59,
    DefineVideoStream = 60,
    VideoFrame = 61,
    DefineFontInfo2 = 62,
    ScriptLimits = 65,
    SetTabIndex = 66,
    FileAttributes = 69,
    PlaceObject3 = 70,
    ImportAssets2 = 71,
    Internal72 = 72,
    DefineFontAlignZones = 73,
    CSMTextSettings = 74,
    DefineFont3 = 75,
    SymbolClass = 76,
    DefineScalingGrid = 78,
    Internal80 = 80,
    Internal81 = 81,
    DoABC = 82,
    DefineShape4 = 83,
    DefineMorphShape2 = 84,
    Internal85 = 85,
    DefineSceneAndFrameLabelData = 86,
    DefineBinaryData = 87,
    StartSound2 = 89,
    DefineBitsJPEG4 = 90,
    DefineFont4 = 91,
    EnableTelemetry = 93,
    Internal94 = 94,
    Internal254 = 254,
    HighRange1022 = 1022,
};

inline const char* tagName(std::uint16_t code) {
    switch (code) {
        case 0:   return "End";
        case 1:   return "ShowFrame";
        case 2:   return "DefineShape";
        case 3:   return "FreeCharacter";
        case 4:   return "PlaceObject";
        case 5:   return "RemoveObject";
        case 6:   return "DefineBits";
        case 7:   return "DefineButton";
        case 8:   return "JPEGTables";
        case 9:   return "SetBackgroundColor";
        case 10:  return "DefineFont";
        case 11:  return "DefineText";
        case 12:  return "DoAction";
        case 13:  return "DefineFontInfo";
        case 14:  return "DefineSound";
        case 15:  return "StartSound";
        case 17:  return "DefineButtonSound";
        case 18:  return "SoundStreamHead";
        case 19:  return "SoundStreamBlock";
        case 20:  return "DefineBitsLossless";
        case 21:  return "DefineBitsJPEG2";
        case 22:  return "DefineShape2";
        case 23:  return "DefineButtonCxform";
        case 26:  return "PlaceObject2";
        case 28:  return "RemoveObject2";
        case 32:  return "DefineShape3";
        case 33:  return "DefineText2";
        case 34:  return "DefineButton2";
        case 35:  return "DefineBitsJPEG3";
        case 36:  return "DefineBitsLossless2";
        case 37:  return "DefineEditText";
        case 39:  return "DefineSprite";
        case 43:  return "FrameLabel";
        case 45:  return "SoundStreamHead2";
        case 46:  return "DefineMorphShape";
        case 48:  return "DefineFont2";
        case 56:  return "ExportAssets";
        case 57:  return "ImportAssets";
        case 59:  return "DoInitAction";
        case 60:  return "DefineVideoStream";
        case 61:  return "VideoFrame";
        case 62:  return "DefineFontInfo2";
        case 65:  return "ScriptLimits";
        case 66:  return "SetTabIndex";
        case 69:  return "FileAttributes";
        case 70:  return "PlaceObject3";
        case 71:  return "ImportAssets2";
        case 72:  return "Internal(72)";
        case 73:  return "DefineFontAlignZones";
        case 74:  return "CSMTextSettings";
        case 75:  return "DefineFont3";
        case 76:  return "SymbolClass";
        case 78:  return "DefineScalingGrid";
        case 80:  return "Internal(80)";
        case 81:  return "Internal(81)";
        case 82:  return "DoABC";
        case 83:  return "DefineShape4";
        case 84:  return "DefineMorphShape2";
        case 85:  return "Internal(85)";
        case 86:  return "DefineSceneAndFrameLabelData";
        case 87:  return "DefineBinaryData";
        case 89:  return "StartSound2";
        case 90:  return "DefineBitsJPEG4";
        case 91:  return "DefineFont4";
        case 93:  return "EnableTelemetry";
        case 94:  return "Internal(94)";
        case 254: return "Internal(254)";
        case 1022: return "HighRange(1022)";
        default:  return "Unknown";
    }
}

// sub_48BA50 中位于第二个 switch（帧/执行类）的标签
inline TagGroup tagGroup(std::uint16_t code) {
    switch (code) {
        case 1: case 4: case 5: case 9: case 12: case 15:
        case 18: case 19: case 26: case 28: case 43:
        case 45: case 59: case 61: case 66: case 70:
        case 72: case 76: case 82: case 89: case 94:
            return TagGroup::Execution;
        case 67: case 68: case 80: case 81: case 254:
            return TagGroup::Ignored;   // 逆向中为空分支
        default:
            return TagGroup::Definition;
    }
}

}  // namespace swf
