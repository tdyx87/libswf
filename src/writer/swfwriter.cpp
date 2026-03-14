#include "writer/swfwriter.h"
#include <fstream>
#include <cstring>
#include <zlib.h>

namespace libswf {

SWFWriter::SWFWriter() 
    : frameRate_(12.0f)
    , frameCount_(1)
    , backgroundColor_(255, 255, 255, 255)
    , hasBackgroundColor_(false)
    , nextCharacterId_(1)
    , currentFrame_(0) {
    // Default frame size: 550x400 pixels (11000x8000 TWIPS)
    frameSize_.xMin = 0;
    frameSize_.yMin = 0;
    frameSize_.xMax = 11000;
    frameSize_.yMax = 8000;
}

SWFWriter::~SWFWriter() = default;

void SWFWriter::setFrameSize(const Rect& size) {
    frameSize_ = size;
}

void SWFWriter::setFrameRate(float fps) {
    frameRate_ = fps;
}

void SWFWriter::setFrameCount(uint16 count) {
    frameCount_ = count;
}

void SWFWriter::setBackgroundColor(const RGBA& color) {
    backgroundColor_ = color;
    hasBackgroundColor_ = true;
}

uint16 SWFWriter::defineShape(const std::vector<ShapeRecord>& records,
                              const std::vector<FillStyle>& fills,
                              const std::vector<LineStyle>& lines) {
    uint16 shapeId = nextCharacterId_++;
    
    // Calculate shape bounds from records
    Rect bounds = {0, 0, 0, 0};
    if (!records.empty()) {
        int32 minX = INT32_MAX, minY = INT32_MAX;
        int32 maxX = INT32_MIN, maxY = INT32_MIN;
        int32 curX = 0, curY = 0;
        
        for (const auto& rec : records) {
            if (rec.type == ShapeRecord::Type::STYLE_CHANGE) {
                curX = rec.moveDeltaX;
                curY = rec.moveDeltaY;
            } else if (rec.type == ShapeRecord::Type::STRAIGHT_EDGE) {
                curX += rec.anchorDeltaX;
                curY += rec.anchorDeltaY;
            } else if (rec.type == ShapeRecord::Type::CURVED_EDGE) {
                curX += rec.controlDeltaX + rec.anchorDeltaX;
                curY += rec.controlDeltaY + rec.anchorDeltaY;
            }
            minX = std::min(minX, curX);
            minY = std::min(minY, curY);
            maxX = std::max(maxX, curX);
            maxY = std::max(maxY, curY);
        }
        bounds.xMin = minX;
        bounds.yMin = minY;
        bounds.xMax = maxX;
        bounds.yMax = maxY;
    }
    
    // Build all shape data in a single BitStreamWriter
    BitStreamWriter shapeBs;
    
    // Shape ID (little-endian)
    shapeBs.writeUB(shapeId & 0xFF, 8);
    shapeBs.writeUB(shapeId >> 8, 8);
    
    // Shape bounds (RECT)
    int32 rectValues[] = {bounds.xMin, bounds.xMax, bounds.yMin, bounds.yMax};
    int rectBits = 1;
    for (int32 v : rectValues) {
        if (v != 0) {
            int bits = 0;
            int32 absV = v < 0 ? -v : v;
            while (absV > 0) {
                absV >>= 1;
                bits++;
            }
            rectBits = std::max(rectBits, bits + 1);
        }
    }
    shapeBs.writeUB(rectBits, 5);
    shapeBs.writeSB(bounds.xMin, rectBits);
    shapeBs.writeSB(bounds.xMax, rectBits);
    shapeBs.writeSB(bounds.yMin, rectBits);
    shapeBs.writeSB(bounds.yMax, rectBits);
    
    // DefineShape2/3 format after RECT:
    // NumFillStyles (5 bits) + NumLineStyles (5 bits)
    // FillStyles array + LineStyles array
    // NumFillBits (4 bits) + NumLineBits (4 bits) - for shape record indexing
    // Shape records
    
    uint8 numFills = static_cast<uint8>(std::min(fills.size(), size_t(31)));  // Max 31 for 5 bits
    uint8 numLines = static_cast<uint8>(std::min(lines.size(), size_t(31)));  // Max 31 for 5 bits
    
    // Calculate bits needed for fill/line style indices in shape records
    int fillBits = (numFills > 0) ? static_cast<int>(std::ceil(std::log2(numFills + 1))) : 0;
    int lineBits = (numLines > 0) ? static_cast<int>(std::ceil(std::log2(numLines + 1))) : 0;
    fillBits = std::max(fillBits, 1);
    lineBits = std::max(lineBits, 1);
    
    // Write NumFillStyles and NumLineStyles (5 bits each)
    shapeBs.writeUB(numFills, 5);
    shapeBs.writeUB(numLines, 5);
    
    // Write FillStyles array
    for (const auto& fill : fills) {
        if (fill.type == FillStyle::Type::SOLID) {
            shapeBs.writeUB(0x00, 8);  // Solid fill type
            shapeBs.writeUB(fill.color.r, 8);
            shapeBs.writeUB(fill.color.g, 8);
            shapeBs.writeUB(fill.color.b, 8);
            shapeBs.writeUB(fill.color.a, 8);  // DefineShape2 has alpha
        }
    }
    
    // Write LineStyles array
    for (const auto& line : lines) {
        shapeBs.writeUB(line.width, 16);
        shapeBs.writeUB(line.color.r, 8);
        shapeBs.writeUB(line.color.g, 8);
        shapeBs.writeUB(line.color.b, 8);
        shapeBs.writeUB(line.color.a, 8);  // DefineShape2 has alpha
    }
    
    // Write NumFillBits and NumLineBits (4 bits each) - for shape records
    shapeBs.writeUB(fillBits, 4);
    shapeBs.writeUB(lineBits, 4);
    
    // Shape records
    for (const auto& rec : records) {
        switch (rec.type) {
            case ShapeRecord::Type::END_SHAPE:
                shapeBs.writeUB(0, 6);  // 6 zero bits = end record
                break;
                
            case ShapeRecord::Type::STYLE_CHANGE: {
                shapeBs.writeUB(0, 1);  // Non-edge record
                bool stateMoveTo = (rec.moveDeltaX != 0 || rec.moveDeltaY != 0);
                uint8 flags = stateMoveTo ? 0x01 : 0;
                shapeBs.writeUB(flags, 5);
                
                if (stateMoveTo) {
                    int moveBits = 1;
                    int32 absMoveX = rec.moveDeltaX < 0 ? -rec.moveDeltaX : rec.moveDeltaX;
                    int32 absMoveY = rec.moveDeltaY < 0 ? -rec.moveDeltaY : rec.moveDeltaY;
                    int32 maxAbs = std::max(absMoveX, absMoveY);
                    while (maxAbs > 0) {
                        maxAbs >>= 1;
                        moveBits++;
                    }
                    shapeBs.writeUB(moveBits, 5);
                    shapeBs.writeSB(rec.moveDeltaX, moveBits);
                    shapeBs.writeSB(rec.moveDeltaY, moveBits);
                }
                break;
            }
            
            case ShapeRecord::Type::STRAIGHT_EDGE: {
                shapeBs.writeUB(1, 1);  // Edge record
                shapeBs.writeUB(1, 1);  // Straight edge
                
                int32 absX = rec.anchorDeltaX < 0 ? -rec.anchorDeltaX : rec.anchorDeltaX;
                int32 absY = rec.anchorDeltaY < 0 ? -rec.anchorDeltaY : rec.anchorDeltaY;
                int32 maxDelta = std::max(absX, absY);
                int numBits = 1;
                while (maxDelta > 0) {
                    maxDelta >>= 1;
                    numBits++;
                }
                numBits = std::max(numBits, 2);
                shapeBs.writeUB(numBits - 2, 4);
                shapeBs.writeUB(1, 1);  // General line
                shapeBs.writeSB(rec.anchorDeltaX, numBits);
                shapeBs.writeSB(rec.anchorDeltaY, numBits);
                break;
            }
            
            case ShapeRecord::Type::CURVED_EDGE: {
                shapeBs.writeUB(1, 1);  // Edge record
                shapeBs.writeUB(0, 1);  // Curved edge
                
                int32 maxDelta = 0;
                int32 vals[] = {rec.controlDeltaX, rec.controlDeltaY, rec.anchorDeltaX, rec.anchorDeltaY};
                for (int32 v : vals) {
                    int32 absV = v < 0 ? -v : v;
                    maxDelta = std::max(maxDelta, absV);
                }
                int numBits = 1;
                while (maxDelta > 0) {
                    maxDelta >>= 1;
                    numBits++;
                }
                numBits = std::max(numBits, 2);
                shapeBs.writeUB(numBits - 2, 4);
                shapeBs.writeSB(rec.controlDeltaX, numBits);
                shapeBs.writeSB(rec.controlDeltaY, numBits);
                shapeBs.writeSB(rec.anchorDeltaX, numBits);
                shapeBs.writeSB(rec.anchorDeltaY, numBits);
                break;
            }
        }
    }
    
    // End shape record
    shapeBs.writeUB(0, 6);
    
    shapeBs.alignByte();
    std::vector<uint8> shapeData = shapeBs.getData();
    
    // Write DefineShape2 tag (tag 22)
    writeTag(22, shapeData);
    
    return shapeId;
}

uint16 SWFWriter::defineImage(const std::vector<uint8>& imageData, bool hasAlpha) {
    uint16 imageId = nextCharacterId_++;
    
    std::vector<uint8> imageTag;
    
    // Image ID (2 bytes)
    imageTag.push_back(static_cast<uint8>(imageId & 0xFF));
    imageTag.push_back(static_cast<uint8>(imageId >> 8));
    
    // Image data
    imageTag.insert(imageTag.end(), imageData.begin(), imageData.end());
    
    // Write DefineBitsJPEG2 tag (tag 21)
    writeTag(21, imageTag);
    
    return imageId;
}

uint16 SWFWriter::defineText(const std::string& text, uint16 fontId,
                               const RGBA& color, uint16 fontSize) {
    uint16 textId = nextCharacterId_++;
    
    std::vector<uint8> textData;
    BitStreamWriter bs;
    
    // Text ID
    bs.writeUB(textId & 0xFF, 8);
    bs.writeUB(textId >> 8, 8);
    
    // Calculate text bounds (simplified - assume single line)
    int32 xMin = 0;
    int32 yMin = 0;
    int32 xMax = static_cast<int32>(text.length() * fontSize * 20);  // Approximate width
    int32 yMax = static_cast<int32>(fontSize * 20 * 1.2f);  // Height with some padding
    
    // Text bounds (RECT)
    int32 rectValues[] = {xMin, xMax, yMin, yMax};
    int rectBits = 1;
    for (int32 v : rectValues) {
        if (v != 0) {
            int bits = 0;
            int32 absV = v < 0 ? -v : v;
            while (absV > 0) {
                absV >>= 1;
                bits++;
            }
            rectBits = std::max(rectBits, bits + 1);
        }
    }
    bs.writeUB(rectBits, 5);
    bs.writeSB(xMin, rectBits);
    bs.writeSB(xMax, rectBits);
    bs.writeSB(yMin, rectBits);
    bs.writeSB(yMax, rectBits);
    
    // Text matrix (identity, positioned at origin)
    bs.writeUB(0, 1);  // No scale
    bs.writeUB(0, 1);  // No rotate
    bs.writeUB(1, 1);  // Has translate
    bs.writeSB(0, 17); // translateX = 0
    bs.writeSB(0, 17); // translateY = 0
    
    // Text records
    // Style change record
    bs.writeUB(1, 1);  // Text record type (1 = style change)
    bs.writeUB(1, 1);  // Has font
    bs.writeUB(1, 1);  // Has color
    bs.writeUB(0, 1);  // No x offset
    bs.writeUB(0, 1);  // No y offset
    bs.writeUB(1, 1);  // Has font height
    
    // Font ID
    bs.writeUB(fontId & 0xFF, 8);
    bs.writeUB(fontId >> 8, 8);
    
    // Text color (RGBA)
    bs.writeUB(color.r, 8);
    bs.writeUB(color.g, 8);
    bs.writeUB(color.b, 8);
    bs.writeUB(color.a, 8);
    
    // Font height (in TWIPS)
    bs.writeUB(fontSize * 20, 16);
    
    // Glyph entry (simplified - just use character codes)
    bs.writeUB(0, 1);  // End of style change
    
    // Glyph text record
    bs.writeUB(1, 1);  // Text record type
    bs.writeUB(static_cast<uint32>(text.length()), 7);  // Glyph count
    
    // Glyph bits and advance bits
    int glyphBits = 8;   // Use 8 bits for glyph index
    int advanceBits = 16; // Use 16 bits for advance
    bs.writeUB(glyphBits, 8);
    bs.writeUB(advanceBits, 8);
    
    // Write glyphs
    for (char c : text) {
        bs.writeUB(static_cast<uint8>(c), glyphBits);  // Glyph index (use ASCII as index)
        bs.writeUB(fontSize * 20 * 0.6f, advanceBits); // Approximate advance
    }
    
    // End of records
    bs.writeUB(0, 1);
    
    bs.alignByte();
    textData = bs.getData();
    
    // Write DefineText tag (tag 11)
    writeTag(11, textData);
    
    return textId;
}

uint16 SWFWriter::defineSprite(uint16 frameCount) {
    uint16 spriteId = nextCharacterId_++;
    
    std::vector<uint8> spriteData;
    
    // Sprite ID
    spriteData.push_back(static_cast<uint8>(spriteId & 0xFF));
    spriteData.push_back(static_cast<uint8>(spriteId >> 8));
    
    // Frame count
    spriteData.push_back(static_cast<uint8>(frameCount & 0xFF));
    spriteData.push_back(static_cast<uint8>(frameCount >> 8));
    
    // End tag
    spriteData.push_back(0x00);
    spriteData.push_back(0x00);
    
    // Write DefineSprite tag (tag 39)
    writeTag(39, spriteData);
    
    return spriteId;
}

void SWFWriter::exportSymbol(uint16 characterId, const std::string& name) {
    std::vector<uint8> exportData;
    
    // Count
    exportData.push_back(0x01);  // 1 export
    exportData.push_back(0x00);
    
    // Character ID
    exportData.push_back(static_cast<uint8>(characterId & 0xFF));
    exportData.push_back(static_cast<uint8>(characterId >> 8));
    
    // Name (null-terminated)
    for (char c : name) {
        exportData.push_back(static_cast<uint8>(c));
    }
    exportData.push_back(0x00);
    
    // Write ExportAssets tag (tag 56)
    writeTag(56, exportData);
}

void SWFWriter::setSymbolClass(uint16 characterId, const std::string& className) {
    std::vector<uint8> symbolData;

    // Number of symbols (UI16) - always 1 for single call
    symbolData.push_back(0x01);
    symbolData.push_back(0x00);

    // Character ID (UI16)
    symbolData.push_back(static_cast<uint8>(characterId & 0xFF));
    symbolData.push_back(static_cast<uint8>(characterId >> 8));

    // Class name (null-terminated string)
    for (char c : className) {
        symbolData.push_back(static_cast<uint8>(c));
    }
    symbolData.push_back(0x00);  // Null terminator

    // Write SymbolClass tag (tag 76)
    writeTag(76, symbolData);
}

void SWFWriter::nextFrame() {
    currentFrame_++;
}

void SWFWriter::showFrame() {
    // Write ShowFrame tag (tag 1, length 0)
    writeTag(1, std::vector<uint8>());
}

void SWFWriter::placeObject(uint16 depth, uint16 characterId, 
                            const Matrix& matrix, const ColorTransform& cx) {
    (void)cx;
    std::vector<uint8> placeData;
    
    // Character ID
    placeData.push_back(static_cast<uint8>(characterId & 0xFF));
    placeData.push_back(static_cast<uint8>(characterId >> 8));
    
    // Depth
    placeData.push_back(static_cast<uint8>(depth & 0xFF));
    placeData.push_back(static_cast<uint8>(depth >> 8));
    
    // Matrix (simplified - identity)
    BitStreamWriter matrixBs;
    matrixBs.writeUB(1, 1);  // Has scale
    matrixBs.writeUB(0, 5);  // NScaleBits = 0
    matrixBs.writeUB(0, 1);  // No rotate/skew
    matrixBs.writeUB(1, 1);  // Has translate
    matrixBs.writeSB(static_cast<int32>(matrix.translateX * 20), 17);
    matrixBs.writeSB(static_cast<int32>(matrix.translateY * 20), 17);
    matrixBs.alignByte();
    std::vector<uint8> matrixData = matrixBs.getData();
    placeData.insert(placeData.end(), matrixData.begin(), matrixData.end());
    
    // Write PlaceObject2 tag (tag 26)
    writeTag(26, placeData);
}

void SWFWriter::removeObject(uint16 depth) {
    std::vector<uint8> removeData;
    
    // Depth (2 bytes, little-endian)
    removeData.push_back(static_cast<uint8>(depth & 0xFF));
    removeData.push_back(static_cast<uint8>(depth >> 8));
    
    // Write RemoveObject2 tag (tag 28)
    writeTag(28, removeData);
}

void SWFWriter::removeObject2(uint16 depth) {
    // RemoveObject2 is the same as RemoveObject in modern SWF
    removeObject(depth);
}

void SWFWriter::writeTagHeader(uint16 tagCode, uint32 tagLength) {
    if (tagLength < 63) {
        uint16 header = (tagCode << 6) | (tagLength & 0x3F);
        data_.push_back(static_cast<uint8>(header & 0xFF));  // Low byte first
        data_.push_back(static_cast<uint8>(header >> 8));
    } else {
        uint16 header = (tagCode << 6) | 0x3F;
        data_.push_back(static_cast<uint8>(header & 0xFF));
        data_.push_back(static_cast<uint8>(header >> 8));
        
        // Long format: 4 bytes for length (little-endian)
        data_.push_back(static_cast<uint8>(tagLength & 0xFF));
        data_.push_back(static_cast<uint8>((tagLength >> 8) & 0xFF));
        data_.push_back(static_cast<uint8>((tagLength >> 16) & 0xFF));
        data_.push_back(static_cast<uint8>((tagLength >> 24) & 0xFF));
    }
}

void SWFWriter::writeTag(uint16 tagCode, const std::vector<uint8>& tagData) {
    writeTagHeader(tagCode, static_cast<uint32>(tagData.size()));
    data_.insert(data_.end(), tagData.begin(), tagData.end());
}

bool SWFWriter::saveToFile(const std::string& filename, uint8 version) {
    auto data = toBinary(version);
    
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        error_ = "Cannot open file: " + filename;
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();
    
    return true;
}

std::vector<uint8> SWFWriter::toBinary(uint8 version) {
    std::vector<uint8> result;
    
    // SWF header
    result.push_back('F');  // Uncompressed signature
    result.push_back('W');
    result.push_back('S');
    result.push_back(version);
    
    // File length placeholder (4 bytes)
    size_t fileLengthPos = result.size();
    result.push_back(0);
    result.push_back(0);
    result.push_back(0);
    result.push_back(0);
    
    // Frame size (RECT) - calculate NBits
    int32 rectValues[] = {frameSize_.xMin, frameSize_.xMax, frameSize_.yMin, frameSize_.yMax};
    int rectBits = 1;
    for (int32 v : rectValues) {
        if (v != 0) {
            int bits = 0;
            int32 absV = v < 0 ? -v : v;
            while (absV > 0) {
                absV >>= 1;
                bits++;
            }
            rectBits = std::max(rectBits, bits + 1);
        }
    }
    
    BitStreamWriter rectWriter;
    rectWriter.writeUB(rectBits, 5);
    rectWriter.writeSB(frameSize_.xMin, rectBits);
    rectWriter.writeSB(frameSize_.xMax, rectBits);
    rectWriter.writeSB(frameSize_.yMin, rectBits);
    rectWriter.writeSB(frameSize_.yMax, rectBits);
    rectWriter.alignByte();
    std::vector<uint8> rectData = rectWriter.getData();
    result.insert(result.end(), rectData.begin(), rectData.end());
    
    // Frame rate (FIXED8) - stored in little-endian
    uint16 frameRateFixed = static_cast<uint16>(frameRate_ * 256);
    result.push_back(static_cast<uint8>(frameRateFixed & 0xFF));
    result.push_back(static_cast<uint8>(frameRateFixed >> 8));
    
    // Frame count - stored in little-endian
    result.push_back(static_cast<uint8>(frameCount_ & 0xFF));
    result.push_back(static_cast<uint8>(frameCount_ >> 8));
    
    // Background color tag
    if (hasBackgroundColor_) {
        std::vector<uint8> bgData;
        bgData.push_back(backgroundColor_.r);
        bgData.push_back(backgroundColor_.g);
        bgData.push_back(backgroundColor_.b);
        // Tag 9 = SetBackgroundColor
        uint16 bgHeader = (9 << 6) | 3;  // tagCode=9, length=3
        result.push_back(static_cast<uint8>(bgHeader & 0xFF));
        result.push_back(static_cast<uint8>(bgHeader >> 8));
        result.insert(result.end(), bgData.begin(), bgData.end());
    }
    
    // Add all accumulated tags from data_
    // data_ already contains complete tag headers and data from writeTag calls
    result.insert(result.end(), data_.begin(), data_.end());
    
    // End tag (tag 0, length 0) - only if not already present
    if (data_.size() < 2 || data_[data_.size()-2] != 0 || data_[data_.size()-1] != 0) {
        result.push_back(0x00);
        result.push_back(0x00);
    }
    
    // Update file length
    uint32 fileLength = static_cast<uint32>(result.size());
    result[fileLengthPos] = static_cast<uint8>(fileLength & 0xFF);
    result[fileLengthPos + 1] = static_cast<uint8>((fileLength >> 8) & 0xFF);
    result[fileLengthPos + 2] = static_cast<uint8>((fileLength >> 16) & 0xFF);
    result[fileLengthPos + 3] = static_cast<uint8>((fileLength >> 24) & 0xFF);
    
    return result;
}

// Helper method implementations
void SWFWriter::writeRect(const Rect& rect) {
    BitStreamWriter bs;
    writeRectBits(bs, rect);
    const auto& data = bs.getData();
    data_.insert(data_.end(), data.begin(), data.end());
}

void SWFWriter::writeRectBits(BitStreamWriter& bs, const Rect& rect) {
    int32 rectValues[] = {rect.xMin, rect.xMax, rect.yMin, rect.yMax};
    int rectBits = 1;
    for (int32 v : rectValues) {
        if (v != 0) {
            int bits = 0;
            int32 absV = v < 0 ? -v : v;
            while (absV > 0) {
                absV >>= 1;
                bits++;
            }
            rectBits = std::max(rectBits, bits + 1);
        }
    }
    bs.writeUB(rectBits, 5);
    bs.writeSB(rect.xMin, rectBits);
    bs.writeSB(rect.xMax, rectBits);
    bs.writeSB(rect.yMin, rectBits);
    bs.writeSB(rect.yMax, rectBits);
}

void SWFWriter::writeMatrix(const Matrix& matrix) {
    BitStreamWriter bs;
    
    // HasScale
    if (matrix.hasScale) {
        bs.writeUB(1, 1);
        // Scale bits - always use 16 bits for simplicity
        bs.writeUB(16, 5);
        // Scale values (16.16 fixed point)
        bs.writeSB(static_cast<int32>(matrix.scaleX * 65536), 32);
        bs.writeSB(static_cast<int32>(matrix.scaleY * 65536), 32);
    } else {
        bs.writeUB(0, 1);
    }
    
    // HasRotate
    if (matrix.hasRotate) {
        bs.writeUB(1, 1);
        // Rotate bits - always use 16 bits
        bs.writeUB(16, 5);
        bs.writeSB(static_cast<int32>(matrix.rotate0 * 65536), 32);
        bs.writeSB(static_cast<int32>(matrix.rotate1 * 65536), 32);
    } else {
        bs.writeUB(0, 1);
    }
    
    // Translate bits
    bs.writeUB(16, 5);
    // Translate (in TWIPS)
    bs.writeSB(static_cast<int32>(matrix.translateX * 20), 32);
    bs.writeSB(static_cast<int32>(matrix.translateY * 20), 32);
    
    // Align to byte boundary
    bs.alignByte();
    
    const auto& data = bs.getData();
    data_.insert(data_.end(), data.begin(), data.end());
}

void SWFWriter::writeColorTransform(const ColorTransform& cx) {
    BitStreamWriter bs;
    
    // HasAddTerms
    bs.writeUB(cx.hasAdd ? 1 : 0, 1);
    // HasMultTerms
    bs.writeUB(1, 1);
    // NBits - use 8 bits for simplicity
    bs.writeUB(8, 4);
    
    // Red mult term
    bs.writeSB(static_cast<int32>(cx.mulR * 256), 8);
    // Green mult term
    bs.writeSB(static_cast<int32>(cx.mulG * 256), 8);
    // Blue mult term
    bs.writeSB(static_cast<int32>(cx.mulB * 256), 8);
    // Alpha mult term
    bs.writeSB(static_cast<int32>(cx.mulA * 256), 8);
    
    if (cx.hasAdd) {
        // Red add term
        bs.writeSB(cx.addR, 8);
        // Green add term
        bs.writeSB(cx.addG, 8);
        // Blue add term
        bs.writeSB(cx.addB, 8);
        // Alpha add term
        bs.writeSB(cx.addA, 8);
    }
    
    bs.alignByte();
    
    const auto& data = bs.getData();
    data_.insert(data_.end(), data.begin(), data.end());
}

void SWFWriter::writeFillStyle(const FillStyle& fill) {
    BitStreamWriter bs;
    writeFillStyleBits(bs, fill);
    bs.alignByte();
    const auto& data = bs.getData();
    data_.insert(data_.end(), data.begin(), data.end());
}

void SWFWriter::writeFillStyleBits(BitStreamWriter& bs, const FillStyle& fill) {
    bs.writeUB(static_cast<uint8>(fill.type), 8);
    if (fill.type == FillStyle::Type::SOLID) {
        bs.writeUB(fill.color.r, 8);
        bs.writeUB(fill.color.g, 8);
        bs.writeUB(fill.color.b, 8);
        bs.writeUB(fill.color.a, 8);
    } else if (fill.type == FillStyle::Type::LINEAR_GRADIENT || 
               fill.type == FillStyle::Type::RADIAL_GRADIENT) {
        // Gradient matrix
        // Simplified - just write identity matrix placeholder
        bs.writeUB(0, 1); // No scale
        bs.writeUB(0, 1); // No rotate
        bs.writeUB(0, 5); // Translate bits = 0
        
        // Gradient spread mode (0 = pad)
        bs.writeUB(0, 2);
        // Gradient interpolation mode (0 = normal)
        bs.writeUB(0, 2);
        // Num gradients
        uint8_t numGradients = std::min(static_cast<uint8_t>(fill.gradientStops.size()), (uint8_t)15);
        bs.writeUB(numGradients, 4);
        
        // Gradient records
        for (uint8_t i = 0; i < numGradients; i++) {
            const auto& gr = fill.gradientStops[i];
            bs.writeUB(gr.ratio, 8);
            bs.writeUB(gr.color.r, 8);
            bs.writeUB(gr.color.g, 8);
            bs.writeUB(gr.color.b, 8);
            bs.writeUB(gr.color.a, 8);
        }
    } else if (fill.type == FillStyle::Type::CLIPPED_BITMAP ||
               fill.type == FillStyle::Type::REPEATING_BITMAP) {
        // Bitmap ID
        bs.writeUB(fill.bitmapId, 16);
        // Bitmap matrix (simplified)
        bs.writeUB(0, 1); // No scale
        bs.writeUB(0, 1); // No rotate
        bs.writeUB(0, 5); // Translate bits = 0
    }
}

void SWFWriter::writeLineStyle(const LineStyle& line) {
    (void)line;
}

void SWFWriter::writeLineStyleBits(BitStreamWriter& bs, const LineStyle& line) {
    bs.writeUB(line.width, 16);
    bs.writeUB(line.color.r, 8);
    bs.writeUB(line.color.g, 8);
    bs.writeUB(line.color.b, 8);
    bs.writeUB(line.color.a, 8);
}

int SWFWriter::calcBitsNeeded(const std::vector<int32_t>& values) {
    int maxBits = 0;
    for (int32_t v : values) {
        if (v == 0) continue;
        int bits = 0;
        int32_t absV = v < 0 ? -v : v;
        while (absV > 0) {
            absV >>= 1;
            bits++;
        }
        maxBits = std::max(maxBits, bits + 1);  // +1 for sign
    }
    return std::max(maxBits, 1);
}

// ShapeBuilder implementations
std::vector<ShapeRecord> ShapeBuilder::createRect(float x, float y, float w, float h) {
    std::vector<ShapeRecord> records;
    
    // Move to start position (in TWIPS)
    ShapeRecord move;
    move.type = ShapeRecord::Type::STYLE_CHANGE;
    move.moveDeltaX = static_cast<int32>(x * 20);
    move.moveDeltaY = static_cast<int32>(y * 20);
    records.push_back(move);
    
    // Right edge
    ShapeRecord right;
    right.type = ShapeRecord::Type::STRAIGHT_EDGE;
    right.anchorDeltaX = static_cast<int32>(w * 20);
    right.anchorDeltaY = 0;
    records.push_back(right);
    
    // Bottom edge
    ShapeRecord bottom;
    bottom.type = ShapeRecord::Type::STRAIGHT_EDGE;
    bottom.anchorDeltaX = 0;
    bottom.anchorDeltaY = static_cast<int32>(h * 20);
    records.push_back(bottom);
    
    // Left edge
    ShapeRecord left;
    left.type = ShapeRecord::Type::STRAIGHT_EDGE;
    left.anchorDeltaX = static_cast<int32>(-w * 20);
    left.anchorDeltaY = 0;
    records.push_back(left);
    
    // Top edge
    ShapeRecord top;
    top.type = ShapeRecord::Type::STRAIGHT_EDGE;
    top.anchorDeltaX = 0;
    top.anchorDeltaY = static_cast<int32>(-h * 20);
    records.push_back(top);
    
    // End
    ShapeRecord end;
    end.type = ShapeRecord::Type::END_SHAPE;
    records.push_back(end);
    
    return records;
}

std::vector<ShapeRecord> ShapeBuilder::createLine(float x0, float y0, float x1, float y1) {
    std::vector<ShapeRecord> records;
    
    // Move to start
    ShapeRecord move;
    move.type = ShapeRecord::Type::STYLE_CHANGE;
    move.moveDeltaX = static_cast<int32>(x0 * 20);
    move.moveDeltaY = static_cast<int32>(y0 * 20);
    records.push_back(move);
    
    // Line to end
    ShapeRecord line;
    line.type = ShapeRecord::Type::STRAIGHT_EDGE;
    line.anchorDeltaX = static_cast<int32_t>((x1 - x0) * 20);
    line.anchorDeltaY = static_cast<int32_t>((y1 - y0) * 20);
    records.push_back(line);
    
    // End
    ShapeRecord end;
    end.type = ShapeRecord::Type::END_SHAPE;
    records.push_back(end);
    
    return records;
}

std::vector<ShapeRecord> ShapeBuilder::createTriangle(float x, float y, float size) {
    std::vector<ShapeRecord> records;
    
    // Move to first point
    ShapeRecord move;
    move.type = ShapeRecord::Type::STYLE_CHANGE;
    move.moveDeltaX = static_cast<int32>(x * 20);
    move.moveDeltaY = static_cast<int32>(y * 20);
    records.push_back(move);
    
    // Line to second point
    ShapeRecord line1;
    line1.type = ShapeRecord::Type::STRAIGHT_EDGE;
    line1.anchorDeltaX = static_cast<int32>(size * 20);
    line1.anchorDeltaY = static_cast<int32>(size * 20);
    records.push_back(line1);
    
    // Line to third point
    ShapeRecord line2;
    line2.type = ShapeRecord::Type::STRAIGHT_EDGE;
    line2.anchorDeltaX = static_cast<int32>(-size * 20);
    line2.anchorDeltaY = 0;
    records.push_back(line2);
    
    // Line back to first point
    ShapeRecord line3;
    line3.type = ShapeRecord::Type::STRAIGHT_EDGE;
    line3.anchorDeltaX = 0;
    line3.anchorDeltaY = static_cast<int32>(-size * 20);
    records.push_back(line3);
    
    // End
    ShapeRecord end;
    end.type = ShapeRecord::Type::END_SHAPE;
    records.push_back(end);
    
    return records;
}

} // namespace libswf
