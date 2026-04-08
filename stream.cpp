#include "stream.h"
#include "math.h"
SWFStream::SWFStream(const uint8_t *data, size_t size) : data_(data), size_(size), position_(0),bitpos_(0) {}

SWFStream::~SWFStream()
{
}

void SWFStream::readSI8(int8_t &value)
{
    value = static_cast<int8_t>(*(data_ + position_));
    position_ += 1;
}

void SWFStream::readSI16(int16_t &value)
{
	uint8_t * p = const_cast<uint8_t*>(data_ + position_);
	value = *reinterpret_cast<int16_t*>(p);
    position_ += 2;
}

void SWFStream::readSI32(int32_t &value)
{
	uint8_t * p = const_cast<uint8_t*>(data_ + position_);
	value = *reinterpret_cast<int32_t*>(p);
    position_ += 4;
}

void SWFStream::readSI8(std::vector<int8_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        value[i] = static_cast<int8_t>(*(data_ + position_));
        position_ += 1;
    }
}

void SWFStream::readSI16(std::vector<int16_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
		uint8_t * p = const_cast<uint8_t*>(data_ + position_);
		value[i] = *reinterpret_cast<int16_t*>(p);
        position_ += 2;
    }
}

void SWFStream::readUI8(uint8_t &value)
{
    value = static_cast<uint8_t>(*(data_ + position_));
    position_ += 1;
}

void SWFStream::readUI16(uint16_t &value)
{
	uint8_t * p = const_cast<uint8_t*>(data_ + position_);
	value = *reinterpret_cast<uint16_t*>(p);
	position_ += 2;
}

void SWFStream::readUI32(uint32_t &value)
{
	uint8_t * p = const_cast<uint8_t*>(data_ + position_);
	value = *reinterpret_cast<uint32_t*>(p);
    position_ += 4;
}

void SWFStream::readUI8(std::vector<uint8_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        value[i] = static_cast<uint8_t>(*(data_ + position_));
        position_ += 1;
    }
}

void SWFStream::readUI16(std::vector<uint16_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
		uint8_t * p = const_cast<uint8_t*>(data_ + position_);
		value[i] = *reinterpret_cast<uint16_t*>(p);
        position_ += 1;
    }
}

void SWFStream::readUI24(std::vector<uint32_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        uint8_t byte1 = static_cast<uint8_t>(*(data_ + position_));
        uint8_t byte2 = static_cast<uint8_t>(*(data_ + position_ + 1));
        uint8_t byte3 = static_cast<uint8_t>(*(data_ + position_ + 2));
        value[i] = byte1 | (byte2 << 8) | (byte3 << 16);
        position_ += 3;
    }
}

void SWFStream::readUI32(std::vector<uint32_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
		uint8_t * p = const_cast<uint8_t*>(data_ + position_);
		value[i] = *reinterpret_cast<uint32_t*>(p);
        position_ += 1;
    }
}

void SWFStream::readUI64(std::vector<uint64_t> &value, size_t count)
{
    value.resize(count);
    for (size_t i = 0; i < count; i++)
    {
		uint8_t * p = const_cast<uint8_t*>(data_ + position_);
		value[i] = *reinterpret_cast<uint64_t*>(p);
        position_ += 1;
    }
}

void SWFStream::readFIXED(float &value)
{
    int32_t fixedValue;
    readSI32(fixedValue);
    value = fixedValue / (double)(1 << 16);
}

void SWFStream::readFIXED8(float &value)
{
    int16_t fixedValue;
    readSI16(fixedValue);
    value = fixedValue / (double)(1 << 8);
}

void SWFStream::readUFIXED8(float &value)
{
    uint16_t fixedValue;
    readUI16(fixedValue);
    value = fixedValue / (double)(1 << 8);
}

void SWFStream::readFLOAT16(float &value)
{
    uint16_t floatValue;
    readUI16(floatValue);
    int val = (int)floatValue;
    int sign = val >> 15;
    int mantissa = sign & 0x3FF;
    int exp = (val >> 10) & 0x1F;
    value = (sign == 1?-1:1)*(float) pow(2,exp)*(1+((mantissa/(float)(1<<10))));
}

void SWFStream::readFLOAT(float &value)
{
    uint32_t floatValue;
    readUI32(floatValue);
    int val = (int)floatValue;
    int sign = floatValue >> 31;
    int mantissa = floatValue & 0x3FFFFF;
    int exp = (floatValue >> 22) & 0xFF;
    value = (sign == 1?-1:1)*(float) pow(2,exp)*(1+((mantissa/(float)(1<<23))));
}

void SWFStream::readDOUBLE(double &value)
{
    std::vector<uint8_t> value8;
    readUI8(value8, 8);
    uint64_t intValue = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        intValue |= static_cast<uint64_t>(value8[i]) << (i * 8);
    }
}

void SWFStream::readEncodedU32(int &value)
{
    int result = *(data_ + position_);
    if (!(result & 0x00000080))
    {
        position_++;
        value = result;
        return;
    }
    result = (result & 0x0000007f) | *(data_ + position_+1) << 7;
    if (!(result & 0x00004000))
    {
        position_ += 2;
        value = result;
        return;
    }
    result = (result & 0x00003fff) | *(data_ + position_+2)  << 14;
    if (!(result & 0x00200000))
    {
        position_ += 3;
        value = result;
        return;
    }
    result = (result & 0x001fffff) | *(data_ + position_+3) << 21;
    if (!(result & 0x10000000))
    {
        position_ += 4;
        value = result;
        return;
    }
    result = (result & 0x0fffffff) | *(data_ + position_+4) << 28;
    position_ += 5;
    value = result;
    return;
}

void SWFStream::readSB(int32_t &value, size_t nbits)
{
    uint32_t uval = 0;
    readUB(uval,nbits);
    int32_t val = (int32_t)(uval);
    int shift = 32-nbits;
    value = (val << shift) >> shift;
}

void SWFStream::readUB(uint32_t &value, size_t nbits)
{
	value = 0;
	if (nbits == 0) return;
    uint8_t cur_byte = *(data_+position_);
    for(int i = 0 ; i < nbits;i++)
    {
        value += (((cur_byte << bitpos_) & (1 << 7)) >> 7) << (nbits- 1 - i);
        bitpos_++;
        if(bitpos_ >= 8)
        {
            position_++;
            bitpos_ = 0;
            cur_byte = *(data_+position_);
        }
    }
}

void SWFStream::readFB(float &value, size_t nbits)
{
    int32_t sb;
     readSB(sb,nbits);
     float fval = (float)sb;
     value = fval / 0x10000;
}

void SWFStream::readRect(RECT &rect)
{
    uint32_t nbits = 0;
    readUB(nbits, 5);
    readSB(rect.x_min, nbits);
    readSB(rect.x_max, nbits);
    readSB(rect.y_min, nbits);
    readSB(rect.y_max, nbits);
    alignByte();
}

void SWFStream::readString(std::string &str)
{
     uint8_t b;
    do
    {
        readUI8(b);
        str.push_back(b);
    } while (b != 0);
}

void SWFStream::readLanguageCode(uint8_t &langCode)
{
    readUI8(langCode);
}

void SWFStream::readRGB(uint8_t &r, uint8_t &g, uint8_t &b)
{
    readUI8(r);
    readUI8(g);
    readUI8(b);
}

void SWFStream::readRGBA(uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
{
    readUI8(r);
    readUI8(g);
    readUI8(b);
    readUI8(a);
}

void SWFStream::readARGB(uint8_t &a, uint8_t &r, uint8_t &g, uint8_t &b)
{
    readUI8(a);
     readUI8(r);
    readUI8(g);
    readUI8(b);
    
}

void SWFStream::readMatrix(MATRIX &matrix)
{
    memset(&matrix,0,sizeof(matrix));
    uint32_t hasScale;
    readUB(hasScale,1);
    if(hasScale == 1)
    {
        uint32_t nbits;
        readUB(nbits,5);
        readFB(matrix.scaleX,nbits);
        readFB(matrix.scaleY,nbits);
    }
    uint32_t hasRotate;
    readUB(hasRotate,1);
    if(hasRotate == 1){
        uint32_t nbits;
        readUB(nbits,5);
        readFB(matrix.rotateskew0,nbits);
        readFB(matrix.rotateskew1,nbits);
    }
    uint32_t nbits;
    readUB(nbits,5);
    readSB(matrix.translateX,nbits);
    readSB(matrix.translateY,nbits);
    alignByte();
}

void SWFStream::readCXForm(CXFORM &cxform)
{
    memset(&cxform,0,sizeof(cxform));
    uint32_t hasaddterm,hasmultterm;
    readUB(hasaddterm,1);
    readUB(hasmultterm,1);
     uint32_t nbits;
    readUB(nbits,4);
    if(hasaddterm == 1)
    {
        readSB(cxform.redMultTerm,nbits);
        readSB(cxform.greenMultTerm,nbits);
        readSB(cxform.blueMultTerm,nbits);
    }
    if(hasmultterm == 1)
    {
        readSB(cxform.redAddTerm,nbits);
        readSB(cxform.greenAddTerm,nbits);
        readSB(cxform.blueAddTerm,nbits);
    }
    alignByte();
}

void SWFStream::readCXFormWithAlpha(CXFORMWITHALPHA &cxform)
{
    memset(&cxform,0,sizeof(cxform));
    uint32_t hasaddterm,hasmultterm;
    readUB(hasaddterm,1);
    readUB(hasmultterm,1);
     uint32_t nbits;
    readUB(nbits,4);
    if(hasaddterm == 1)
    {
        readSB(cxform.redMultTerm,nbits);
        readSB(cxform.greenMultTerm,nbits);
        readSB(cxform.blueMultTerm,nbits);
        readSB(cxform.alphaMultTerm,nbits);
    }
    if(hasmultterm == 1)
    {
        readSB(cxform.redAddTerm,nbits);
        readSB(cxform.greenAddTerm,nbits);
        readSB(cxform.blueAddTerm,nbits);
        readSB(cxform.alphaAddTerm,nbits);
    }
    alignByte();
}

void SWFStream::readTag(uint16_t &tagCode, uint32_t &tagLength, std::function<void(const uint8_t *data, uint32_t taglength,uint16_t tagcode)> tagdata)
{
    uint16_t tagCodeAndLength;
    readUI16(tagCodeAndLength);
    tagCode = tagCodeAndLength >> 6;
    uint32_t length = tagCodeAndLength & 0x3F;
    if (length == 0x3F)
    {
        readUI32(tagLength);
    }
    else
    {
        tagLength = length;
    }

    tagdata(data_+position_,tagLength,tagCode);

    position_ += tagLength;
}

uint32_t SWFStream::GetCurPos()
{
    return position_;
}

void SWFStream::CopyStream(std::shared_ptr<SWFStream> stream, uint32_t start, uint32_t end)
{
    stream.reset(new SWFStream(data_+start,end-start));
}

void SWFStream::alignByte()
{
    if(bitpos_ != 0)
    {
        bitpos_ = 0;
        position_ ++;
    }
}
