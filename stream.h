#pragma once
#include <stdint.h>
#include <vector>
#include <memory>
#include <functional>
struct RECT
{
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
};

struct MATRIX{
    float scaleX;
    float scaleY;
    float rotateskew0;
    float rotateskew1;
    int32_t translateX;
    int32_t translateY;
};

struct CXFORM{
    int32_t redMultTerm;
    int32_t greenMultTerm;
    int32_t blueMultTerm;
    int32_t redAddTerm;
    int32_t greenAddTerm;
    int32_t blueAddTerm;
};


struct CXFORMWITHALPHA{
    int32_t redMultTerm;
    int32_t greenMultTerm;
    int32_t blueMultTerm;
    int32_t alphaMultTerm;
    int32_t redAddTerm;
    int32_t greenAddTerm;
    int32_t blueAddTerm;
    int32_t alphaAddTerm;
};

typedef float FIXED;
typedef float FIXED8;

class SWFStream
{
public:
    SWFStream(const uint8_t *data, size_t size);
    virtual ~SWFStream();
    void readSI8(int8_t& value);
    void readSI16(int16_t& value);
    void readSI32(int32_t& value);
    void readSI8(std::vector<int8_t>& value, size_t count);
    void readSI16(std::vector<int16_t>& value,size_t count);
    void readUI8(uint8_t& value);
    void readUI16(uint16_t& value);
    void readUI32(uint32_t& value);
    void readUI8(std::vector<uint8_t>& value,size_t count);
    void readUI16(std::vector<uint16_t>& value,size_t count);
    void readUI24(std::vector<uint32_t>& value,size_t count);
    void readUI32(std::vector<uint32_t>& value, size_t count);
    void readUI64(std::vector<uint64_t>& value,size_t count);
    void readFIXED(float& value);
    void readFIXED8(float& value);
    void readUFIXED8(float& value);
    void readFLOAT16(float& value);
    void readFLOAT(float& value);
    void readDOUBLE(double& value);
    void readEncodedU32(int& value);
    void readSB(int32_t& value,size_t nbits);
    void readUB(uint32_t& value,size_t nbits);
    void readFB(float& value,size_t nbits);
    void readRect(RECT& rect);
    void readString(std::string& str);
    void readLanguageCode(uint8_t& langCode);
    void readRGB(uint8_t& r, uint8_t& g, uint8_t& b);
    void readRGBA(uint8_t& r, uint8_t& g,uint8_t& b, uint8_t& a);
    void readARGB( uint8_t& a,uint8_t& r, uint8_t& g,uint8_t& b);
    void readMatrix(MATRIX& matrix);
    void readCXForm(CXFORM& cxform);
    void readCXFormWithAlpha(CXFORMWITHALPHA& cxform);
    void readTag(uint16_t& tagCode,uint32_t& tagLength , std::function<void(const uint8_t *data, uint32_t taglength, uint16_t tagcode)> tagdata);
    uint32_t GetCurPos();
    void CopyStream(std::shared_ptr<SWFStream> stream,uint32_t start, uint32_t end);
private:
    void alignByte();
protected:
    const uint8_t *data_;
    size_t size_;
    size_t position_;
    uint8_t bitpos_;
};