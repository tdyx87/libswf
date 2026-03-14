#ifndef LIBSWF_PARSER_SWFTAG_H
#define LIBSWF_PARSER_SWFTAG_H

#include "common/types.h"
#include "common/bitstream.h"
#include <variant>
#include <memory>

namespace libswf {

// Forward declarations
struct TagBase;
struct ShapeTag;
struct ImageTag;
struct TextTag;
struct EditTextTag;
struct FontTag;
struct ActionTag;
struct ABCFileTag;
struct SpriteTag;
struct VideoStreamTag;
struct VideoFrameTag;

// Tag data variants
using TagData = std::variant<
    std::monostate,           // Unknown tag
    std::unique_ptr<ShapeTag>,
    std::unique_ptr<ImageTag>,
    std::unique_ptr<TextTag>,
    std::unique_ptr<EditTextTag>,
    std::unique_ptr<FontTag>,
    std::unique_ptr<ActionTag>,
    std::unique_ptr<ABCFileTag>,
    std::unique_ptr<SpriteTag>,
    std::unique_ptr<VideoStreamTag>,
    std::unique_ptr<VideoFrameTag>
>;

// Base tag structure
struct TagBase {
    uint16 code;
    SWFTagType type;
    uint32 length;
    uint32 offset;  // Offset in file
    
    TagBase(uint16 c, uint32 l, uint32 o = 0) 
        : code(c), type(static_cast<SWFTagType>(c & 0x3FF)), length(l), offset(o) {}
    
    virtual ~TagBase() = default;
    virtual std::string toString() const {
        return "Tag: code=" + std::to_string(code) + 
               ", type=" + std::to_string(static_cast<uint16>(type)) +
               ", length=" + std::to_string(length);
    }
};

// Clip action record - stores event/action pairs
struct ClipActionRecord {
    uint32 eventFlags;
    uint8 keyCode;
    std::vector<uint8> actions;

    ClipActionRecord() : eventFlags(0), keyCode(0) {}
};

// Display list item - represents a placed object on the display list
struct DisplayItem {
    enum class Type {
        PLACE,      // Place a new character
        MOVE,       // Move/modify existing character (no character ID)
        REMOVE      // Remove character at depth
    };

    Type type;
    uint16 characterId;
    uint16 depth;
    uint16 clipDepth;
    Matrix matrix;
    ColorTransform cxform;
    std::string instanceName;
    uint8 blendMode;
    uint8 ratio;  // For morph shapes (0-255)
    bool hasMatrix;
    bool hasCxform;
    bool hasRatio;
    bool hasBlendMode;
    bool hasClipActions;
    std::vector<Filter> filters;
    std::vector<ClipActionRecord> clipActions;

    DisplayItem()
        : type(Type::PLACE), characterId(0), depth(0), clipDepth(0),
          blendMode(0), ratio(0),
          hasMatrix(false), hasCxform(false), hasRatio(false),
          hasBlendMode(false), hasClipActions(false) {}
};

// Frame state - stores display list operations for a frame
struct Frame {
    uint16 frameNumber = 0;
    std::string label;
    std::vector<DisplayItem> displayListOps;  // PlaceObject/RemoveObject operations
    
    // Find a display item by depth
    DisplayItem* findByDepth(uint16 depth) {
        for (auto& item : displayListOps) {
            if (item.depth == depth) {
                return &item;
            }
        }
        return nullptr;
    }
    
    // Remove display item by depth
    void removeByDepth(uint16 depth) {
        displayListOps.erase(
            std::remove_if(displayListOps.begin(), displayListOps.end(),
                [depth](const DisplayItem& item) { return item.depth == depth; }),
            displayListOps.end());
    }
};

// Shape data
// Gradient stop for gradient fills
struct GradientStop {
    uint8 ratio;  // 0-255
    RGBA color;
    
    GradientStop() : ratio(0), color(0, 0, 0, 255) {}
    GradientStop(uint8 r, const RGBA& c) : ratio(r), color(c) {}
};

struct FillStyle {
    enum class Type {
        SOLID = 0,
        LINEAR_GRADIENT = 0x10,
        RADIAL_GRADIENT = 0x12,
        FOCAL_RADIAL_GRADIENT = 0x13,
        REPEATING_BITMAP = 0x40,
        CLIPPED_BITMAP = 0x41,
        NON_SMOOTHED_REPEATING_BITMAP = 0x42,
        NON_SMOOTHED_CLIPPED_BITMAP = 0x43
    };
    
    Type type;
    RGBA color;                    // For SOLID fill
    Matrix gradientMatrix;         // For GRADIENT fills
    float focalPoint;              // For FOCAL_RADIAL_GRADIENT (-1.0 to 1.0)
    std::vector<GradientStop> gradientStops;  // For GRADIENT fills
    uint16 bitmapId;               // For BITMAP fills
    Matrix bitmapMatrix;           // For BITMAP fills
    
    FillStyle() : type(Type::SOLID), focalPoint(0.0f), bitmapId(0) {}
    
    bool isGradient() const {
        return type == Type::LINEAR_GRADIENT || 
               type == Type::RADIAL_GRADIENT ||
               type == Type::FOCAL_RADIAL_GRADIENT;
    }
    
    bool isBitmap() const {
        return static_cast<uint8>(type) >= 0x40;
    }
};

struct LineStyle {
    uint16 width;
    RGBA color;
    
    LineStyle() : width(0), color(0, 0, 0, 255) {}
};

struct ShapeRecord {
    enum class Type {
        END_SHAPE = 0,      // End of shape records
        STYLE_CHANGE = 1,   // Style change (moveTo, new styles)
        STRAIGHT_EDGE = 2,  // Straight line
        CURVED_EDGE = 3     // Quadratic bezier curve
    };
    
    Type type;
    int32 moveDeltaX, moveDeltaY;
    int32 controlDeltaX, controlDeltaY;
    int32 anchorDeltaX, anchorDeltaY;
    
    // For STYLE_CHANGE records
    int fillStyle0 = 0;  // Left fill style index (0 = none)
    int fillStyle1 = 0;  // Right fill style index (0 = none)
    int lineStyle = 0;   // Line style index (0 = none)
    bool newStyles = false;  // Flag for new fill/line style array
    
    ShapeRecord() : type(Type::END_SHAPE), 
                   moveDeltaX(0), moveDeltaY(0),
                   controlDeltaX(0), controlDeltaY(0),
                   anchorDeltaX(0), anchorDeltaY(0) {}
};

struct ShapeTag : public TagBase {
    uint16 shapeId;
    Rect bounds;
    std::vector<FillStyle> fillStyles;
    std::vector<LineStyle> lineStyles;
    std::vector<ShapeRecord> records;
    bool hasAlpha;  // SWF version 3+
    
    ShapeTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), shapeId(0), hasAlpha(false) {}
};

// Image data
struct ImageTag : public TagBase {
    uint16 imageId;
    enum class Format {
        JPEG,
        LOSSLESS,
        LOSSLESS_ALPHA
    };
    Format format;
    std::vector<uint8> imageData;
    uint8 alphaDataOffset;  // For JPEG3
    
    ImageTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), imageId(0), format(Format::JPEG), alphaDataOffset(0) {}
};

// Font data
struct GlyphEntry {
    std::vector<int32> shapeRecords;
};

struct KerningRecord {
    uint16 code1, code2;
    int16 adjustment;
};

struct FontTag : public TagBase {
    uint16 fontId;
    bool isFont3;  // Font3 for Unicode fonts
    std::string fontName;
    bool hasLayout;
    
    // Glyph data
    std::vector<uint16> codeTable;
    std::vector<GlyphEntry> glyphs;
    std::vector<int32> advanceTable;
    std::vector<Rect> boundsTable;
    std::vector<KerningRecord> kerningTable;
    
    // Layout data
    int16 ascent, descent, leading;
    
    FontTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), fontId(0), isFont3(false), 
          hasLayout(false), ascent(0), descent(0), leading(0) {}
};

// Glyph entry for text records
struct TextGlyphEntry {
    uint32 glyphIndex;
    int32 glyphAdvance;
};

// Text data
struct TextRecord {
    Matrix fontMatrix;
    int32 xOffset, yOffset;
    uint16 fontId;
    RGBA textColor;
    uint16 fontHeight;
    std::string text;
    std::vector<TextGlyphEntry> glyphs;

    TextRecord() : xOffset(0), yOffset(0), fontId(0),
                   fontHeight(12), textColor(0, 0, 0, 255) {}
};

struct TextTag : public TagBase {
    uint16 textId;
    Rect bounds;
    std::vector<TextRecord> records;
    
    TextTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), textId(0) {}
};

// ActionScript tag
struct ActionTag : public TagBase {
    std::vector<uint8> actionData;
    std::vector<uint8> data;  // Action data for Button2
    uint16 conditions = 0;    // Button action conditions (DefineButton2)
    
    ActionTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), conditions(0) {}
};

// ABC file (ActionScript 3 Bytecode)
struct ABCFileTag : public TagBase {
    uint32 flags;  // LAZY_INIT_FLAG
    std::string name;  // Symbol class name
    std::vector<uint8> abcData;
    
    ABCFileTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), flags(0) {}
};

// Sound format types
enum class SoundFormat {
    UNCOMPRESSED_NATIVE_ENDIAN = 0,
    ADPCM = 1,
    MP3 = 2,
    UNCOMPRESSED_LITTLE_ENDIAN = 3,
    NELLYMOSER_16KHZ = 4,
    NELLYMOSER_8KHZ = 5,
    NELLYMOSER = 6,
    SPEEX = 11
};

// Sound rate types
enum class SoundRate {
    KHZ_5_5 = 0,
    KHZ_11 = 1,
    KHZ_22 = 2,
    KHZ_44 = 3
};

// DefineSound tag
struct SoundTag : public TagBase {
    uint16 soundId;
    SoundFormat format;
    SoundRate rate;
    bool is16Bit;
    bool isStereo;
    uint32 sampleCount;
    std::vector<uint8> soundData;
    
    SoundTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), soundId(0), format(SoundFormat::UNCOMPRESSED_NATIVE_ENDIAN),
          rate(SoundRate::KHZ_5_5), is16Bit(false), isStereo(false), sampleCount(0) {}
};

// Sound stream info
struct SoundStreamHeadTag : public TagBase {
    SoundFormat playbackFormat;
    SoundRate playbackRate;
    bool playbackIs16Bit;
    bool playbackIsStereo;
    
    SoundFormat streamFormat;
    SoundRate streamRate;
    bool streamIs16Bit;
    bool streamIsStereo;
    uint16 sampleCount;  // Average samples per block
    uint16 latencySeek;  // For MP3
    
    SoundStreamHeadTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), 
          playbackFormat(SoundFormat::UNCOMPRESSED_NATIVE_ENDIAN),
          playbackRate(SoundRate::KHZ_5_5), playbackIs16Bit(false), playbackIsStereo(false),
          streamFormat(SoundFormat::UNCOMPRESSED_NATIVE_ENDIAN),
          streamRate(SoundRate::KHZ_5_5), streamIs16Bit(false), streamIsStereo(false),
          sampleCount(0), latencySeek(0) {}
};

// Sound stream block
struct SoundStreamBlockTag : public TagBase {
    std::vector<uint8> blockData;
    
    SoundStreamBlockTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o) {}
};

// Start/Stop sound
struct StartSoundTag : public TagBase {
    uint16 soundId;
    bool stopPlayback;
    bool noMultiple;
    bool hasEnvelope;
    bool hasLoop;
    bool hasOutPoint;
    bool hasInPoint;
    uint32 inPoint;
    uint32 outPoint;
    uint16 loopCount;
    
    StartSoundTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), soundId(0), stopPlayback(false), noMultiple(false),
          hasEnvelope(false), hasLoop(false), hasOutPoint(false), hasInPoint(false),
          inPoint(0), outPoint(0), loopCount(0) {}
};

// Button states
enum class ButtonState : uint8 {
    UP = 0x01,
    OVER = 0x02,
    DOWN = 0x04,
    HIT_TEST = 0x08
};

// Button record (character in button)
struct ButtonRecord {
    uint16 characterId;
    uint16 depth;
    Matrix matrix;
    ColorTransform colorTransform;
    ButtonState states;
    bool hasFilterList;
    bool hasBlendMode;
    BlendMode blendMode;
};

// DefineButton / DefineButton2 tag
struct ButtonTag : public TagBase {
    uint16 buttonId;
    bool isButton2;  // DefineButton2 has actions
    bool trackAsMenu;
    
    std::vector<ButtonRecord> records;
    std::vector<uint8> actionData;  // For DefineButton2 (DefineButtonCxform)
    
    // Button actions (DefineButton2)
    std::vector<std::unique_ptr<ActionTag>> actions;
    
    ButtonTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), buttonId(0), isButton2(false), trackAsMenu(false) {}
};

// Button sound info
struct ButtonSoundTag : public TagBase {
    uint16 buttonId;
    struct SoundInfo {
        uint16 soundId;
        bool stopPlayback;
        bool noMultiple;
        bool hasEnvelope;
        bool hasLoops;
        bool hasOutPoint;
        bool hasInPoint;
        uint32 inPoint;
        uint32 outPoint;
        uint16 loopCount;
    };
    SoundInfo overUpToIdle;
    SoundInfo idleToOverUp;
    SoundInfo overUpToOverDown;
    SoundInfo overDownToOverUp;
    
    ButtonSoundTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), buttonId(0) {}
};

// Exported asset
struct ExportedAsset {
    uint16 characterId;
    std::string name;
    
    ExportedAsset(uint16 id, const std::string& n) 
        : characterId(id), name(n) {}
};

// Imported asset
struct ImportedAsset {
    std::string url;
    uint16 exportSymbolId;
    std::string name;
    
    ImportedAsset(const std::string& u, uint16 id, const std::string& n)
        : url(u), exportSymbolId(id), name(n) {}
};

// SWF Tag
struct SWFTag {
    std::unique_ptr<TagBase> tag;
    
    SWFTag() = default;
    SWFTag(std::unique_ptr<TagBase> t) : tag(std::move(t)) {}
    
    template<typename T>
    T* as() {
        return dynamic_cast<T*>(tag.get());
    }
    
    template<typename T>
    const T* as() const {
        return dynamic_cast<const T*>(tag.get());
    }
    
    uint16 code() const { return tag ? tag->code : 0; }
    SWFTagType type() const { return tag ? tag->type : SWFTagType::END; }
    uint32 length() const { return tag ? tag->length : 0; }
};

// Morph fill style (for shape tweening)
struct MorphFillStyle {
    FillStyle startStyle;
    FillStyle endStyle;
};

// Morph line style (for shape tweening)
struct MorphLineStyle {
    LineStyle startStyle;
    LineStyle endStyle;
};

// Morph shape record (for shape tweening)
struct MorphShapeRecord {
    ShapeRecord startRecord;
    ShapeRecord endRecord;
};

// DefineMorphShape tag (tag 46)
struct MorphShapeTag : public TagBase {
    uint16 characterId;
    Rect startBounds;
    Rect endBounds;
    std::vector<MorphFillStyle> fillStyles;
    std::vector<MorphLineStyle> lineStyles;
    std::vector<MorphShapeRecord> records;
    
    MorphShapeTag(uint16 c, uint32 l, uint32 o = 0)
        : TagBase(c, l, o), characterId(0) {
        type = SWFTagType::DEFINE_MORPH_SHAPE;
    }
    
    // Interpolate between start and end shapes based on ratio (0.0 - 1.0)
    ShapeTag interpolate(float ratio) const {
        ShapeTag result(2, 0, 0);
        result.shapeId = characterId;
        
        // Interpolate bounds
        result.bounds.xMin = static_cast<int32>(startBounds.xMin + 
            (endBounds.xMin - startBounds.xMin) * ratio);
        result.bounds.xMax = static_cast<int32>(startBounds.xMax + 
            (endBounds.xMax - startBounds.xMax) * ratio);
        result.bounds.yMin = static_cast<int32>(startBounds.yMin + 
            (endBounds.yMin - startBounds.yMin) * ratio);
        result.bounds.yMax = static_cast<int32>(startBounds.yMax + 
            (endBounds.yMax - startBounds.yMax) * ratio);
        
        // Interpolate records
        for (const auto& morphRec : records) {
            ShapeRecord rec;
            rec.type = morphRec.startRecord.type;
            rec.moveDeltaX = static_cast<int32>(morphRec.startRecord.moveDeltaX + 
                (morphRec.endRecord.moveDeltaX - morphRec.startRecord.moveDeltaX) * ratio);
            rec.moveDeltaY = static_cast<int32>(morphRec.startRecord.moveDeltaY + 
                (morphRec.endRecord.moveDeltaY - morphRec.startRecord.moveDeltaY) * ratio);
            rec.anchorDeltaX = static_cast<int32>(morphRec.startRecord.anchorDeltaX + 
                (morphRec.endRecord.anchorDeltaX - morphRec.startRecord.anchorDeltaX) * ratio);
            rec.anchorDeltaY = static_cast<int32>(morphRec.startRecord.anchorDeltaY + 
                (morphRec.endRecord.anchorDeltaY - morphRec.startRecord.anchorDeltaY) * ratio);
            rec.controlDeltaX = static_cast<int32>(morphRec.startRecord.controlDeltaX + 
                (morphRec.endRecord.controlDeltaX - morphRec.startRecord.controlDeltaX) * ratio);
            rec.controlDeltaY = static_cast<int32>(morphRec.startRecord.controlDeltaY + 
                (morphRec.endRecord.controlDeltaY - morphRec.startRecord.controlDeltaY) * ratio);
            rec.fillStyle0 = morphRec.startRecord.fillStyle0;
            rec.fillStyle1 = morphRec.startRecord.fillStyle1;
            rec.lineStyle = morphRec.startRecord.lineStyle;
            result.records.push_back(rec);
        }
        
        return result;
    }
};

// Video codec types
enum class VideoCodec : uint8 {
    H263 = 2,           // Sorenson H.263
    SCREEN_VIDEO = 3,   // Screen video
    VP6 = 4,            // On2 VP6
    VP6_ALPHA = 5,      // On2 VP6 with alpha channel
    SCREEN_V2 = 6,      // Screen video version 2
    AVC = 7             // H.264/AVC
};

// DefineVideoStream tag (tag 60)
struct VideoStreamTag : public TagBase {
    uint16 characterId;
    uint16 numFrames;
    uint16 width;
    uint16 height;
    VideoCodec codec;
    bool smoothDeblocking;
    // VideoFlagsReserved (1 bit)
    // VideoFlagsDeblocking (2 bits)
    // VideoFlagsSmoothing (1 bit)
    
    VideoStreamTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), characterId(0), numFrames(0), 
          width(0), height(0), codec(VideoCodec::H263), 
          smoothDeblocking(false) {}
};

// VideoFrame tag (tag 61)
struct VideoFrameTag : public TagBase {
    uint16 streamId;
    uint16 frameNum;
    std::vector<uint8> videoData;
    
    VideoFrameTag(uint16 c, uint32 l, uint32 o = 0) 
        : TagBase(c, l, o), streamId(0), frameNum(0) {}
};

// DefineEditText tag (tag 37) - Input text field
struct EditTextTag : public TagBase {
    uint16 characterId;
    Rect bounds;
    bool wordWrap;
    bool multiline;
    bool password;
    bool readOnly;
    bool autoSize;
    bool noSelect;
    bool border;
    bool html;
    bool useOutlines;

    uint16 fontId;
    uint16 fontHeight;
    RGBA textColor;
    uint16 maxLength;
    uint8 align;
    uint16 leftMargin;
    uint16 rightMargin;
    uint16 indent;
    int16 leading;
    std::string variableName;
    std::string initialText;

    EditTextTag(uint16 c, uint32 l, uint32 o = 0)
        : TagBase(c, l, o), characterId(0), wordWrap(false), multiline(false),
          password(false), readOnly(false), autoSize(false), noSelect(false),
          border(false), html(false), useOutlines(false), fontId(0),
          fontHeight(0), textColor(RGBA(0, 0, 0, 255)), maxLength(0),
          align(0), leftMargin(0), rightMargin(0), indent(0), leading(0) {}
};

// Sprite (MovieClip) - container of other characters
struct SpriteTag : public TagBase {
    uint16 spriteId;
    uint16 frameCount;
    std::vector<std::string> frameLabels;
    std::vector<uint16> frameLabelNumbers;

    // Frame data - stores display list state for each frame
    std::vector<Frame> frames;

    SpriteTag(uint16 c, uint32 l, uint32 o = 0)
        : TagBase(c, l, o), spriteId(0), frameCount(0) {}
};

} // namespace libswf

#endif // LIBSWF_PARSER_SWFTAG_H
