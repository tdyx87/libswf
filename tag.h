#pragma once
#include <stdint.h>
#include "stream.h"
#include "tagdefine.h"
class Tag
{
public:
    Tag(const uint8_t*data, const uint32_t length, TagType tagcode);
    ~Tag();
    virtual void process() = 0;
protected:
    std::shared_ptr<SWFStream> stream_;
    TagType tag_code_;
    uint32_t tagLength_;
};

std::shared_ptr<Tag> CreateTag(const uint8_t *data,uint32_t taglength, uint16_t tagcode );


class FileAttributes : public Tag
{
    struct data
    {
       union{
            uint32_t d;
            struct s{
                uint8_t reserved1:1;
                uint8_t usedirectblit:1;
                uint8_t usegpu:1;
                uint8_t hasmetadata:1;
                uint8_t actionscript3:1;
                uint8_t reserved2:2;
                uint8_t usenetwork:1;
                uint8_t s[3];
            }s;
        };
    };
public:
    FileAttributes(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    data d;
};

class Metadata : public Tag
{
public:
    Metadata(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    std::string metadata;
};


class SetBackgroundColor : public Tag
{
    struct RGB{
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };
public:
    SetBackgroundColor(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    RGB BackgroundColor;
};


class DefineSceneAndFrameLabelData : public Tag
{
    struct Scene{
        int Offset;
        std::string Name;
    };
    struct Frame{
        int FrameNum;
        std::string FrameLabel;
    };
public:
    DefineSceneAndFrameLabelData(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    int SceneCount;
    std::vector<Scene> scenes;
    int FrameLabelCount;
    std::vector<Frame> frames;
};

class SoundStreamHead : public Tag
{
public:
    SoundStreamHead(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
   uint32_t Reserved;
   uint32_t PlaybackSoundRate;
   uint32_t PlaybackSoundSize;
   uint32_t PlaybackSoundType;
   uint32_t StreamSoundCompression;
   uint32_t StreamSoundRate;
   uint32_t StreamSoundSize;
   uint32_t StreamSoundType;
   uint16_t StreamSoundSampleCount;
   int16_t LatencySeek;
};


class SoundStreamHead2 : public Tag
{
public:
    SoundStreamHead2(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
   uint32_t Reserved;
   uint32_t PlaybackSoundRate;
   uint32_t PlaybackSoundSize;
   uint32_t PlaybackSoundType;
   uint32_t StreamSoundCompression;
   uint32_t StreamSoundRate;
   uint32_t StreamSoundSize;
   uint32_t StreamSoundType;
   uint16_t StreamSoundSampleCount;
   int16_t LatencySeek;
};

struct Color
{
    uint32_t RGBA;
    union{
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
};


struct GRADRECORD
{
    uint8_t Ratio;
    Color color;
};

struct GRADIENT
{
    uint32_t SpreadMode;
    uint32_t InterpolationMode;
    uint32_t NumGradients;
    std::vector<GRADRECORD> GradientRecords;
};


struct FOCALGRADIENT
{
    uint32_t SpreadMode;
    uint32_t InterpolationMode;
    uint32_t NumGradients;
    std::vector<GRADRECORD> GradientRecords;
    FIXED8 FocalPoint;
};

struct FILLSTYLE
{
    uint8_t FillStyleType;
    Color color;
    MATRIX GradientMatrix;
    GRADIENT Gradient;
    uint16_t BitmapId;
    MATRIX BitmapMatrix;
};

struct FILLSTYLEARRAY
{
    uint8_t FillStyleCount;
    uint16_t FillStyleCountExtended;
    std::vector<FILLSTYLE> FillStyles;

};

struct LINESTYLE
{
    uint16_t Width;
    Color color;
};

struct LINESTYLE2
{
    uint16_t Width;
    uint32_t StartCapStyle;
    uint32_t JoinStyle;
    uint32_t HasFillFlag;
    uint32_t NoHScaleFlag;
    uint32_t NoVScaleFlag;
    uint32_t PixelHintingFlag;
    uint32_t Reserved;
    uint32_t NoClose;
    uint32_t EndCapStyle;
    uint16_t MiterLimitFactor;
    Color color;
    FILLSTYLE FillType;
};

struct LINESTYLEARRAY
{
    uint8_t LineStyleCount;
    uint16_t LineStyleCountExtended;
    std::vector<LINESTYLE> LineStyles;
};

struct SHAPERECORD
{
    uint32_t TypeFlag;
};

struct ENDSHAPERECORD:SHAPERECORD
{
    uint32_t TypeFlag;
    uint32_t EndOfShape;
};

struct STYLECHANGERECORD:SHAPERECORD
{
    uint32_t TypeFlag;
    uint32_t StateNewStyles;
    uint32_t StateLineStyle;
    uint32_t StateFillStyle1;
    uint32_t StateFillStyle0;
    uint32_t StateMoveTo;
    uint32_t MoveBits;
    int32_t MoveDeltaX;
    int32_t MoveDeltaY;
    uint32_t FillStyle0;
    uint32_t FillStyle1;
    uint32_t LineStyle;
    FILLSTYLEARRAY FillStyles;
    LINESTYLEARRAY LineStyles;
    uint32_t NumFillBits;
    uint32_t NumLineBits;
};

struct STRAIGHTEDGERECORD:SHAPERECORD{
    uint32_t TypeFlag;
    uint32_t StraightFlag;
    uint32_t NumBits;
    uint32_t GeneralLineFlag;
    int32_t GeneralLineDeltaX;
    int32_t GeneralLineDeltaY;
    int32_t VertLineFlag;
    int32_t VertLineDeltaX;
    int32_t VertLineDeltaY;
};

struct CURVEDEDGERECORD:SHAPERECORD{
    uint32_t TypeFlag;
    uint32_t StraightFlag;
    uint32_t NumBits;
    int32_t ControlDeltaX;
    int32_t ControlDeltaY;
    int32_t AnchorDeltaX;
    int32_t AnchorDeltaY;
};

struct SHAPEWITHSTYLE
{
    FILLSTYLEARRAY FillStyles;
    LINESTYLEARRAY LineStyles;
    uint32_t NumFillBits;
    uint32_t NumLineBits;
    std::vector<SHAPERECORD> ShapeRecords;
};

class DefineShape3 : public Tag
{
public:
    DefineShape3(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    uint16_t ShapeId;
    RECT ShapeBounds;
    SHAPEWITHSTYLE Shapes;
};

class DefineSprite : public Tag
{
public:
    DefineSprite(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    uint16_t SpriteID;
    uint16_t FrameCount;
    std::vector<std::shared_ptr<Tag>> ControlTags;
};

struct ACTIONRECORD
{
    uint8_t ActionCode;
    uint16_t Length;
};

struct CLIPEVENTFLAGS
{
    uint32_t ClipEventKeyUp;
    uint32_t ClipEventKeyDown;
    uint32_t ClipEventMouseUp;
    uint32_t ClipEventMouseDown;
    uint32_t ClipEventMouseMove;
    uint32_t ClipEventUnload;
    uint32_t ClipEventEnterFrame;
    uint32_t ClipEventLoad;
    uint32_t ClipEventDragOver;
    uint32_t ClipEventRollOut;
    uint32_t ClipEventRollOver;
    uint32_t ClipEventReleaseOutside;
    uint32_t ClipEventRelease;
    uint32_t ClipEventPress;
    uint32_t ClipEventInitialize;
    uint32_t ClipEventData;
    uint32_t Reserved;
    uint32_t ClipEventConstruct;
    uint32_t ClipEventKeyPress;
    uint32_t ClipEventDragOut;
    uint32_t Reserved2;
};

struct CLIPACTIONRECORD
{
    CLIPEVENTFLAGS EventFlags;
    uint32_t ActionRecordSize;
    uint8_t KeyCode;
    std::vector<ACTIONRECORD> Actions;
};




struct CLIPACTIONS{
    uint16_t Reserved;
    CLIPEVENTFLAGS AllEventFlags;
    std::vector<CLIPACTIONRECORD> ClipActionRecords;
    uint32_t ClipActionEndFlag;
};

class PlaceObject2 : public Tag
{
public:
    PlaceObject2(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    uint32_t PlaceFlagHasClipActions;
    uint32_t PlaceFlagHasClipDepth;
    uint32_t PlaceFlagHasName;
    uint32_t PlaceFlagHasRatio;
    uint32_t PlaceFlagHasColorTransform;
    uint32_t PlaceFlagHasMatrix;
    uint32_t PlaceFlagHasCharactor;
    uint32_t PlaceFlagMove;
    uint16_t Depth;
    uint16_t CharactorId;
    MATRIX Matrix;
    CXFORMWITHALPHA ColorTransform;
    uint16_t Ratio;
    std::string Name;
    uint16_t ClipDepth;
    CLIPACTIONS ClipActions;
};


class ShowFrame : public Tag
{
public:
    ShowFrame(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    
};


class End : public Tag
{
public:
    End(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    
};


class DefineBitsJPEG3 : public Tag
{
public:
    DefineBitsJPEG3(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    uint16_t CharacterID;
    uint32_t AlphaDataOffset;
    std::vector<uint8_t> ImageData;
    std::vector<uint8_t> BitmapAlphaData;
};


class DefineSound : public Tag
{
public:
    DefineSound(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
    uint16_t SoundId;
    uint32_t SoundFormat;
    uint32_t SoundRate;
    uint32_t SoundSize;
    uint32_t SoundType;
    uint32_t SoundSampleCount;
    std::vector<uint8_t> SoundData;
};


class DefineShape : public Tag
{
public:
    DefineShape(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
     uint16_t ShapeId;
     RECT ShapeBounds;
     SHAPEWITHSTYLE Shapes;
};


class DefineShape2 : public Tag
{
public:
    DefineShape2(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
     uint16_t ShapeId;
     RECT ShapeBounds;
     SHAPEWITHSTYLE Shapes;
};

class DefineShape4 : public Tag
{
public:
    DefineShape4(const uint8_t*data, const uint32_t length);
public:
    void process() override;
private:
     uint16_t ShapeId;
     RECT ShapeBounds;
     SHAPEWITHSTYLE Shapes;
};
