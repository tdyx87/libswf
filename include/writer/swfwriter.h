#ifndef LIBSWF_WRITER_SWFWRITER_H
#define LIBSWF_WRITER_SWFWRITER_H

#include "common/types.h"
#include "common/bitstream.h"
#include "parser/swfheader.h"
#include "parser/swftag.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace libswf {

// SWF Writer for creating SWF files programmatically
class SWFWriter {
public:
    SWFWriter();
    ~SWFWriter();
    
    // Document properties
    void setFrameSize(const Rect& size);
    void setFrameRate(float fps);
    void setFrameCount(uint16 count);
    void setBackgroundColor(const RGBA& color);
    
    // Shape creation
    uint16 defineShape(const std::vector<ShapeRecord>& records,
                       const std::vector<FillStyle>& fills,
                       const std::vector<LineStyle>& lines);
    
    // Image creation
    uint16 defineImage(const std::vector<uint8>& imageData, bool hasAlpha = false);
    
    // Text creation
    uint16 defineText(const std::string& text, uint16 fontId, 
                      const RGBA& color, uint16 fontSize);
    
    // MovieClip (Sprite) creation
    uint16 defineSprite(uint16 frameCount);
    
    // Export/Linkage
    void exportSymbol(uint16 characterId, const std::string& name);
    void setSymbolClass(uint16 characterId, const std::string& className);
    
    // Frame control
    void nextFrame();
    void showFrame();
    
    // Place objects
    void placeObject(uint16 depth, uint16 characterId, 
                     const Matrix& matrix = Matrix(),
                     const ColorTransform& cx = ColorTransform());
    
    // Remove objects
    void removeObject(uint16 depth);
    void removeObject2(uint16 depth);  // RemoveObject2 tag
    
    // Save to file
    bool saveToFile(const std::string& filename, uint8 version = 10);
    
    // Get binary data
    std::vector<uint8> toBinary(uint8 version = 10);
    
    // Error handling
    const std::string& getError() const { return error_; }

private:
    std::vector<uint8> data_;
    std::string error_;
    
    Rect frameSize_;
    float frameRate_;
    uint16 frameCount_;
    RGBA backgroundColor_;
    bool hasBackgroundColor_;
    
    uint16 nextCharacterId_;
    uint16 currentFrame_;
    
    // Tag writing helpers
    void writeTagHeader(uint16 tagCode, uint32 tagLength);
    void writeTag(uint16 tagCode, const std::vector<uint8>& tagData);
    
    // Helper methods
    void writeRect(const Rect& rect);
    void writeRectBits(BitStreamWriter& bs, const Rect& rect);
    void writeMatrix(const Matrix& matrix);
    void writeColorTransform(const ColorTransform& cx);
    void writeFillStyle(const FillStyle& fill);
    void writeFillStyleBits(BitStreamWriter& bs, const FillStyle& fill);
    void writeLineStyle(const LineStyle& line);
    void writeLineStyleBits(BitStreamWriter& bs, const LineStyle& line);
    
    int calcBitsNeeded(const std::vector<int32_t>& values);
};

// Convenience factory for common shapes
class ShapeBuilder {
public:
    static std::vector<ShapeRecord> createRect(float x, float y, float w, float h);
    static std::vector<ShapeRecord> createLine(float x0, float y0, float x1, float y1);
    static std::vector<ShapeRecord> createTriangle(float x, float y, float size);
};

} // namespace libswf

#endif // LIBSWF_WRITER_SWFWRITER_H
