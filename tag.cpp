#include "tag.h"

Tag::Tag(const uint8_t *data, const uint32_t length, TagType tagcode) : stream_(new SWFStream(data, length)), tag_code_(tagcode), tagLength_(length)
{
}

Tag::~Tag()
{
}

FileAttributes::FileAttributes(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_FILEATTRIBUTES)
{
}

void FileAttributes::process()
{
    stream_->readUI32(d.d);
}

std::shared_ptr<Tag> CreateTag(const uint8_t *data, uint32_t taglength, uint16_t tagcode)
{
    TagType tagtype = static_cast<TagType>(tagcode);
    std::shared_ptr<Tag> res;
    switch (tagtype)
    {
    case TagType::TAG_FILEATTRIBUTES:
        res.reset(new FileAttributes(data, taglength));
        break;
    case TagType::TAG_METADATA:
        res.reset(new Metadata(data, taglength));
        break;
    case TagType::TAG_SETBACKGROUNDCOLOR:
        res.reset(new SetBackgroundColor(data, taglength));
        break;
    case TagType::TAG_DEFINESCENEANDFRAMELABELDATA:
        res.reset(new DefineSceneAndFrameLabelData(data, taglength));
        break;
    case TagType::TAG_SOUNDSTREAMHEAD:
        res.reset(new SoundStreamHead(data, taglength));
        break;
    case TagType::TAG_SOUNDSTREAMHEAD2:
        res.reset(new SoundStreamHead2(data, taglength));
        break;
    case TagType::TAG_DEFINESHAPE3:
        res.reset(new DefineShape3(data, taglength));
        break;
	case TagType::TAG_DEFINESHAPE:
		res.reset(new DefineShape(data, taglength));
		break;
    case TagType::TAG_DEFINESPRITE:
        res.reset(new DefineSprite(data, taglength));
        break;
    case TagType::TAG_PLACEOBJECT2:
        res.reset(new PlaceObject2(data,taglength));
    break;
    case TagType::TAG_SHOWFRAME:
        res.reset(new ShowFrame(data,taglength));
    break;
    case TagType::TAG_END:
    res.reset(new End(data,taglength));
    break;
    case TagType::TAG_DEFINEBITSJPEG3:
    res.reset(new DefineBitsJPEG3(data,taglength));
    break;
    case TagType::TAG_DEFINESOUND:
    res.reset(new DefineSound(data,taglength));
    break;
    };

    return res;
}

Metadata::Metadata(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_METADATA)
{
}

void Metadata::process()
{
    stream_->readString(metadata);
}

SetBackgroundColor::SetBackgroundColor(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_SETBACKGROUNDCOLOR)
{
}

void SetBackgroundColor::process()
{
    stream_->readRGB(BackgroundColor.r, BackgroundColor.g, BackgroundColor.b);
}

DefineSceneAndFrameLabelData::DefineSceneAndFrameLabelData(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_DEFINESCENEANDFRAMELABELDATA)
{
}

void DefineSceneAndFrameLabelData::process()
{
    stream_->readEncodedU32(SceneCount);
    for (int i = 0; i < SceneCount; i++)
    {
        Scene scene;
        stream_->readEncodedU32(scene.Offset);
        stream_->readString(scene.Name);
        scenes.push_back(scene);
    }
    stream_->readEncodedU32(FrameLabelCount);
    for (int i = 0; i < FrameLabelCount; i++)
    {
        Frame frame;
        stream_->readEncodedU32(frame.FrameNum);
        stream_->readString(frame.FrameLabel);
    }
}

SoundStreamHead::SoundStreamHead(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_SETBACKGROUNDCOLOR)
{
}

void SoundStreamHead::process()
{
    stream_->readUB(Reserved, 4);
    stream_->readUB(PlaybackSoundRate, 2);
    stream_->readUB(PlaybackSoundSize, 1);
    stream_->readUB(PlaybackSoundType, 1);
    stream_->readUB(StreamSoundCompression, 4);
    stream_->readUB(StreamSoundRate, 2);
    stream_->readUB(StreamSoundSize, 1);
    stream_->readUB(StreamSoundType, 1);
    stream_->readUI16(StreamSoundSampleCount);
    if (StreamSoundCompression == 2)
    {
        stream_->readSI16(LatencySeek);
    }
}

SoundStreamHead2::SoundStreamHead2(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_SETBACKGROUNDCOLOR)
{
}

void SoundStreamHead2::process()
{
    stream_->readUB(Reserved, 4);
    stream_->readUB(PlaybackSoundRate, 2);
    stream_->readUB(PlaybackSoundSize, 1);
    stream_->readUB(PlaybackSoundType, 1);
    stream_->readUB(StreamSoundCompression, 4);
    stream_->readUB(StreamSoundRate, 2);
    stream_->readUB(StreamSoundSize, 1);
    stream_->readUB(StreamSoundType, 1);
    stream_->readUI16(StreamSoundSampleCount);
    if (StreamSoundCompression == 2)
    {
        stream_->readSI16(LatencySeek);
    }
}

DefineShape3::DefineShape3(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_DEFINESHAPE3)
{
}

void DefineShape3::process()
{
    stream_->readUI16(ShapeId);
    stream_->readRect(ShapeBounds);

    // shapewithstyle
    // fillstylearray
    uint8_t fillstylecount;
    uint16_t fillstylecountextended;
    stream_->readUI8(fillstylecount);
    if (fillstylecount == 0xFF)
        stream_->readUI16(fillstylecountextended);
    if (fillstylecount > 0)
    {
        for (int i = 0; i < fillstylecount; i++)
        {
            uint8_t fillstyletype;
            stream_->readUI8(fillstyletype);
            if (fillstyletype == 0x00)
            {
                // shape3 是 rgba shape1 / shape2 是 rgb
                Color color;
                stream_->readRGBA(color.r, color.g, color.b, color.a);
            }
            else if (fillstyletype == 0x10 || fillstyletype == 0x12 || fillstyletype == 0x13)
            {
                MATRIX GradientMatrix;
                stream_->readMatrix(GradientMatrix);
                uint32_t SpreadMode, InterpolationMode, NumGradients;
                stream_->readUB(SpreadMode, 2);
                stream_->readUB(InterpolationMode, 2);
                stream_->readUB(NumGradients, 4);
                if (NumGradients > 0)
                {
                    for (int j = 0; j < NumGradients; j++)
                    {
                        uint8_t Ratio;
                        Color color;
                        stream_->readUI8(Ratio);
                        stream_->readRGBA(color.r, color.g, color.b, color.a);
                    }
                }
                if (fillstyletype == 0x13)
                {
                    // 读focalgradient
                    FIXED8 FocalPoint;
                    stream_->readFIXED8(FocalPoint);
                }
            }
            else if (fillstyletype == 0x40 || fillstyletype == 0x41 || fillstyletype == 0x42 || fillstyletype == 0x43)
            {
                uint16_t BitmapId;
                stream_->readUI16(BitmapId);
                MATRIX BitmapMatrix;
                stream_->readMatrix(BitmapMatrix);
            }
        }
    }
    // linestylearray
    uint8_t linestylecount;
    uint16_t linestylecountextended;
    stream_->readUI8(linestylecount);
    if (linestylecount == 0xFF)
    {
        stream_->readUI16(linestylecountextended);
    }
    for (int i = 0; i < linestylecount; i++)
    {
        uint16_t Width;
        Color color;
        stream_->readUI16(Width);
        stream_->readRGBA(color.r, color.g, color.b, color.a);
    }
    // numfillbits
    uint32_t NumFillBits, NumLineBits;
    stream_->readUB(NumFillBits, 4);
    // numlinebits
    stream_->readUB(NumLineBits, 4);
    // shaperecord
    uint32_t TypeFlag, EndOfShape;
    do
    {
        /* code */
        stream_->readUB(TypeFlag, 1);
        if (TypeFlag == 0)
        {
            uint32_t StateNewStyles = 0, StateLineStyle = 0, StateFillStyle1 = 0, StateFillStyle0 = 0, StateMoveTo = 0;
            stream_->readUB(StateNewStyles, 1);
            stream_->readUB(StateLineStyle, 1);
            stream_->readUB(StateFillStyle1, 1);
            stream_->readUB(StateFillStyle0, 1);
            stream_->readUB(StateMoveTo, 1);
            if (StateNewStyles == 0 && StateLineStyle == 0 && StateFillStyle1 == 0 && StateFillStyle0 == 0 && StateMoveTo == 0)
            {
                break;
            }
            if (StateMoveTo == 1)
            {
                uint32_t MoveBits;
                int32_t MoveDeltaX, MoveDeltaY;
                stream_->readUB(MoveBits, 5);
                stream_->readSB(MoveDeltaX, MoveBits);
                stream_->readSB(MoveDeltaY, MoveBits);
            }
            if (StateFillStyle0 == 1)
            {
                uint32_t fillstyle0;
                stream_->readUB(fillstyle0, NumFillBits);
            }
            if (StateFillStyle1 == 1)
            {
                uint32_t fillstyle1;
                stream_->readUB(fillstyle1, NumFillBits);
            }
            if (StateLineStyle == 1)
            {
                uint32_t linestyle;
                stream_->readUB(linestyle, NumLineBits);
            }
            if (StateNewStyles == 1)
            {
                FILLSTYLEARRAY FillStyles; // 读取
                uint8_t fillstylecount;
                uint16_t fillstylecountextended;
                stream_->readUI8(fillstylecount);
                if (fillstylecount == 0xFF)
                    stream_->readUI16(fillstylecountextended);
                if (fillstylecount > 0)
                {
                    for (int i = 0; i < fillstylecount; i++)
                    {
                        uint8_t fillstyletype;
                        stream_->readUI8(fillstyletype);
                        if (fillstyletype == 0x00)
                        {
                            // shape3 是 rgba shape1 / shape2 是 rgb
                            Color color;
                            stream_->readRGBA(color.r, color.g, color.b, color.a);
                        }
                        else if (fillstyletype == 0x10 || fillstyletype == 0x12 || fillstyletype == 0x13)
                        {
                            MATRIX GradientMatrix;
                            stream_->readMatrix(GradientMatrix);
                            uint32_t SpreadMode, InterpolationMode, NumGradients;
                            stream_->readUB(SpreadMode, 2);
                            stream_->readUB(InterpolationMode, 2);
                            stream_->readUB(NumGradients, 4);
                            if (NumGradients > 0)
                            {
                                for (int j = 0; j < NumGradients; j++)
                                {
                                    uint8_t Ratio;
                                    Color color;
                                    stream_->readUI8(Ratio);
                                    stream_->readRGBA(color.r, color.g, color.b, color.a);
                                }
                            }
                            if (fillstyletype == 0x13)
                            {
                                // 读focalgradient
                                FIXED8 FocalPoint;
                                stream_->readFIXED8(FocalPoint);
                            }
                        }
                        else if (fillstyletype == 0x40 || fillstyletype == 0x41 || fillstyletype == 0x42 || fillstyletype == 0x43)
                        {
                            uint16_t BitmapId;
                            stream_->readUI16(BitmapId);
                            MATRIX BitmapMatrix;
                            stream_->readMatrix(BitmapMatrix);
                        }
                    }
                }
                uint8_t linestylecount;
                uint16_t linestylecountextended;
                stream_->readUI8(linestylecount);
                if (linestylecount == 0xFF)
                {
                    stream_->readUI16(linestylecountextended);
                }
                for (int i = 0; i < linestylecount; i++)
                {
                    uint16_t Width;
                    Color color;
                    stream_->readUI16(Width);
                    stream_->readRGBA(color.r, color.g, color.b, color.a);
                }
                uint32_t NumFillBits, NumLineBits;
                stream_->readUB(NumFillBits, 4);
                // numlinebits
                stream_->readUB(NumLineBits, 4);
            }
        }
        else if (TypeFlag == 1)
        {
            uint32_t straightFlag;
            stream_->readUB(straightFlag, 1);
            if (straightFlag == 1)
            {
                uint32_t NumBits, GeneralLineFlag;
                stream_->readUB(NumBits, 4);
                stream_->readUB(GeneralLineFlag, 1);
                if (GeneralLineFlag == 1)
                {
                    int32_t DeltaX, DeltaY;
                    stream_->readSB(DeltaX, NumBits + 2);
                    stream_->readSB(DeltaY, NumBits + 2);
                }
                else
                {
                    uint32_t VertLineFlag;
                    stream_->readUB(VertLineFlag, 1);
                    if (VertLineFlag == 1)
                    {
                        int32_t  DeltaY;
                        stream_->readSB(DeltaY, NumBits + 2);
                    }
					else
					{
						int32_t DeltaX;
						stream_->readSB(DeltaX, NumBits + 2);
					}
                }
            }
            else if (straightFlag == 0)
            {
                uint32_t NumBits;
                stream_->readUB(NumBits, 4);
                int32_t ControlDeltaX, ControlDeltaY, AnchorDeltaX, AnchorDeltaY;
                stream_->readSB(ControlDeltaX, NumBits + 2);
                stream_->readSB(ControlDeltaY, NumBits + 2);
                stream_->readSB(AnchorDeltaX, NumBits + 2);
                stream_->readSB(AnchorDeltaY, NumBits + 2);
            }
        }
    } while (1);
}

DefineSprite::DefineSprite(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_DEFINESHAPE3)
{
}

void DefineSprite::process()
{
    stream_->readUI16(SpriteID);
    stream_->readUI16(FrameCount);

    while (1)
    {
        uint16_t tagCode;
        uint32_t tagLength;
        stream_->readTag(tagCode, tagLength, [&](const uint8_t *data, uint32_t taglength, uint16_t tagcode)
                         {
                  std::shared_ptr<Tag> tag = CreateTag(data,taglength,tagcode);
                  if(tag)
                  {
                     tag->process();
                    ControlTags.emplace_back(tag);
                  }
                  });
        if (tagCode == 0)
        {
            break;
        }
    }
}

PlaceObject2::PlaceObject2(const uint8_t *data, const uint32_t length) : Tag(data, length, TagType::TAG_DEFINESHAPE3)
{
}

void PlaceObject2::process()
{
    stream_->readUB(PlaceFlagHasClipActions, 1);
    stream_->readUB(PlaceFlagHasClipDepth, 1);
    stream_->readUB(PlaceFlagHasName, 1);
    stream_->readUB(PlaceFlagHasRatio, 1);
    stream_->readUB(PlaceFlagHasColorTransform, 1);
    stream_->readUB(PlaceFlagHasMatrix, 1);
    stream_->readUB(PlaceFlagHasCharactor, 1);
    stream_->readUB(PlaceFlagMove, 1);
    stream_->readUI16(Depth);
    if (PlaceFlagHasCharactor == 1)
        stream_->readUI16(CharactorId);
    if (PlaceFlagHasMatrix == 1)
        stream_->readMatrix(Matrix);
    if (PlaceFlagHasColorTransform == 1)
        stream_->readCXFormWithAlpha(ColorTransform);
    if (PlaceFlagHasRatio == 1)
        stream_->readUI16(Ratio);
    if (PlaceFlagHasName == 1)
        stream_->readString(Name);
    if (PlaceFlagHasClipDepth == 1)
        stream_->readUI16(ClipDepth);
    if (PlaceFlagHasClipActions == 1)
    {
        stream_->readUI16(ClipActions.Reserved);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventKeyUp, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventKeyDown, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventMouseUp, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventMouseDown, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventMouseMove, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventUnload, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventEnterFrame, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventLoad, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventDragOver, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventRollOut, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventRollOver, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventReleaseOutside, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventRelease, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventPress, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventInitialize, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventData, 1);
        stream_->readUB(ClipActions.AllEventFlags.Reserved, 5);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventConstruct, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventKeyPress, 1);
        stream_->readUB(ClipActions.AllEventFlags.ClipEventDragOut, 1);
        stream_->readUB(ClipActions.AllEventFlags.Reserved2, 8);
    }
}

ShowFrame::ShowFrame(const uint8_t *data, const uint32_t length): Tag(data, length, TagType::TAG_SHOWFRAME)
{
}

void ShowFrame::process()
{
}

End::End(const uint8_t *data, const uint32_t length): Tag(data, length, TagType::TAG_END)
{
}

void End::process()
{
}

DefineBitsJPEG3::DefineBitsJPEG3(const uint8_t *data, const uint32_t length): Tag(data, length, TagType::TAG_DEFINEBITSJPEG3)
{
}

void DefineBitsJPEG3::process()
{
    stream_->readUI16(CharacterID);
    stream_->readUI32(AlphaDataOffset);
    stream_->readUI8(ImageData,AlphaDataOffset);
	//压缩数据
    stream_->readUI8(BitmapAlphaData,tagLength_-AlphaDataOffset-6);
}

DefineSound::DefineSound(const uint8_t *data, const uint32_t length): Tag(data, length, TagType::TAG_DEFINEBITSJPEG3)
{
}

void DefineSound::process()
{
    stream_->readUI16(SoundId);
    stream_->readUB(SoundFormat,4);
    stream_->readUB(SoundRate,2);
    stream_->readUB(SoundSize,1);
    stream_->readUB(SoundType,1);
    stream_->readUI32(SoundSampleCount);
    stream_->readUI8(SoundData,tagLength_-2-1-4);
}

DefineShape::DefineShape(const uint8_t *data, const uint32_t length): Tag(data, length, TagType::TAG_DEFINEBITSJPEG3)
{

}

void DefineShape::process()
{
    stream_->readUI16(ShapeId);
    stream_->readRect(ShapeBounds);

    // shapewithstyle
    // fillstylearray
    uint8_t fillstylecount;
    uint16_t fillstylecountextended;
    stream_->readUI8(fillstylecount);
    if (fillstylecount == 0xFF)
        stream_->readUI16(fillstylecountextended);
    if (fillstylecount > 0)
    {
        for (int i = 0; i < fillstylecount; i++)
        {
            uint8_t fillstyletype;
            stream_->readUI8(fillstyletype);
            if (fillstyletype == 0x00)
            {
                // shape3 是 rgba shape1 / shape2 是 rgb
                Color color;
                stream_->readRGB(color.r, color.g, color.b);
            }
            else if (fillstyletype == 0x10 || fillstyletype == 0x12 || fillstyletype == 0x13)
            {
                MATRIX GradientMatrix;
                stream_->readMatrix(GradientMatrix);
                uint32_t SpreadMode, InterpolationMode, NumGradients;
                stream_->readUB(SpreadMode, 2);
                stream_->readUB(InterpolationMode, 2);
                stream_->readUB(NumGradients, 4);
                if (NumGradients > 0)
                {
                    for (int j = 0; j < NumGradients; j++)
                    {
                        uint8_t Ratio;
                        Color color;
                        stream_->readUI8(Ratio);
                        stream_->readRGB(color.r, color.g, color.b);
                    }
                }
                if (fillstyletype == 0x13)
                {
                    // 读focalgradient
                    FIXED8 FocalPoint;
                    stream_->readFIXED8(FocalPoint);
                }
            }
            else if (fillstyletype == 0x40 || fillstyletype == 0x41 || fillstyletype == 0x42 || fillstyletype == 0x43)
            {
                uint16_t BitmapId;
                stream_->readUI16(BitmapId);
                MATRIX BitmapMatrix;
                stream_->readMatrix(BitmapMatrix);
            }
        }
    }
    // linestylearray
    uint8_t linestylecount;
    uint16_t linestylecountextended;
    stream_->readUI8(linestylecount);
    if (linestylecount == 0xFF)
    {
        stream_->readUI16(linestylecountextended);
    }
    for (int i = 0; i < linestylecount; i++)
    {
        uint16_t Width;
        Color color;
        stream_->readUI16(Width);
        stream_->readRGBA(color.r, color.g, color.b, color.a);
    }
    // numfillbits
    uint32_t NumFillBits, NumLineBits;
    stream_->readUB(NumFillBits, 4);
    // numlinebits
    stream_->readUB(NumLineBits, 4);
    // shaperecord
    uint32_t TypeFlag, EndOfShape;
    do
    {
        /* code */
        stream_->readUB(TypeFlag, 1);
        if (TypeFlag == 0)
        {
            uint32_t StateNewStyles = 0, StateLineStyle = 0, StateFillStyle1 = 0, StateFillStyle0 = 0, StateMoveTo = 0;
            stream_->readUB(StateNewStyles, 1);
            stream_->readUB(StateLineStyle, 1);
            stream_->readUB(StateFillStyle1, 1);
            stream_->readUB(StateFillStyle0, 1);
            stream_->readUB(StateMoveTo, 1);
            if (StateNewStyles == 0 && StateLineStyle == 0 && StateFillStyle1 == 0 && StateFillStyle0 == 0 && StateMoveTo == 0)
            {
                break;
            }
            if (StateMoveTo == 1)
            {
                uint32_t MoveBits;
                int32_t MoveDeltaX, MoveDeltaY;
                stream_->readUB(MoveBits, 5);
                stream_->readSB(MoveDeltaX, MoveBits);
                stream_->readSB(MoveDeltaY, MoveBits);
            }
            if (StateFillStyle0 == 1)
            {
                uint32_t fillstyle0;
                stream_->readUB(fillstyle0, NumFillBits);
            }
            if (StateFillStyle1 == 1)
            {
                uint32_t fillstyle1;
                stream_->readUB(fillstyle1, NumFillBits);
            }
            if (StateLineStyle == 1)
            {
                uint32_t linestyle;
                stream_->readUB(linestyle, NumLineBits);
            }
            if (StateNewStyles == 1)
            {
                FILLSTYLEARRAY FillStyles; // 读取
                uint8_t fillstylecount;
                uint16_t fillstylecountextended;
                stream_->readUI8(fillstylecount);
                if (fillstylecount == 0xFF)
                    stream_->readUI16(fillstylecountextended);
                if (fillstylecount > 0)
                {
                    for (int i = 0; i < fillstylecount; i++)
                    {
                        uint8_t fillstyletype;
                        stream_->readUI8(fillstyletype);
                        if (fillstyletype == 0x00)
                        {
                            // shape3 是 rgba shape1 / shape2 是 rgb
                            Color color;
                            stream_->readRGBA(color.r, color.g, color.b, color.a);
                        }
                        else if (fillstyletype == 0x10 || fillstyletype == 0x12 || fillstyletype == 0x13)
                        {
                            MATRIX GradientMatrix;
                            stream_->readMatrix(GradientMatrix);
                            uint32_t SpreadMode, InterpolationMode, NumGradients;
                            stream_->readUB(SpreadMode, 2);
                            stream_->readUB(InterpolationMode, 2);
                            stream_->readUB(NumGradients, 4);
                            if (NumGradients > 0)
                            {
                                for (int j = 0; j < NumGradients; j++)
                                {
                                    uint8_t Ratio;
                                    Color color;
                                    stream_->readUI8(Ratio);
                                    stream_->readRGB(color.r, color.g, color.b);
                                }
                            }
                            if (fillstyletype == 0x13)
                            {
                                // 读focalgradient
                                FIXED8 FocalPoint;
                                stream_->readFIXED8(FocalPoint);
                            }
                        }
                        else if (fillstyletype == 0x40 || fillstyletype == 0x41 || fillstyletype == 0x42 || fillstyletype == 0x43)
                        {
                            uint16_t BitmapId;
                            stream_->readUI16(BitmapId);
                            MATRIX BitmapMatrix;
                            stream_->readMatrix(BitmapMatrix);
                        }
                    }
                }
                uint8_t linestylecount;
                uint16_t linestylecountextended;
                stream_->readUI8(linestylecount);
                if (linestylecount == 0xFF)
                {
                    stream_->readUI16(linestylecountextended);
                }
                for (int i = 0; i < linestylecount; i++)
                {
                    uint16_t Width;
                    Color color;
                    stream_->readUI16(Width);
                    stream_->readRGB(color.r, color.g, color.b);
                }
                uint32_t NumFillBits, NumLineBits;
                stream_->readUB(NumFillBits, 4);
                // numlinebits
                stream_->readUB(NumLineBits, 4);
            }
        }
        else if (TypeFlag == 1)
        {
            uint32_t straightFlag;
            stream_->readUB(straightFlag, 1);
            if (straightFlag == 1)
            {
                uint32_t NumBits, GeneralLineFlag;
                stream_->readUB(NumBits, 4);
                stream_->readUB(GeneralLineFlag, 1);
                if (GeneralLineFlag == 1)
                {
                    int32_t DeltaX, DeltaY;
                    stream_->readSB(DeltaX, NumBits + 2);
                    stream_->readSB(DeltaY, NumBits + 2);
                }
                else
                {
					uint32_t VertLineFlag;
                    stream_->readUB(VertLineFlag, 1);
                    if (VertLineFlag == 1)
                    {
                        int32_t DeltaX, DeltaY;
                        
                        stream_->readSB(DeltaY, NumBits + 2);
                    }
					else
					{
						int32_t DeltaX, DeltaY;
						stream_->readSB(DeltaX, NumBits + 2);
					}
                }
            }
            else if (straightFlag == 0)
            {
                uint32_t NumBits;
                stream_->readUB(NumBits, 4);
                int32_t ControlDeltaX, ControlDeltaY, AnchorDeltaX, AnchorDeltaY;
                stream_->readSB(ControlDeltaX, NumBits + 2);
                stream_->readSB(ControlDeltaY, NumBits + 2);
                stream_->readSB(AnchorDeltaX, NumBits + 2);
                stream_->readSB(AnchorDeltaY, NumBits + 2);
            }
        }
    } while (1);
}
