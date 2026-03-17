#include "parser/swfparser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <zlib.h>

namespace libswf {

// Helper functions for reading multi-byte values
static inline uint16 readU16(const uint8* data, size_t& pos) {
    uint16 val = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    return val;
}

static inline uint32 readU32(const uint8* data, size_t& pos) {
    uint32 val = data[pos] | (data[pos + 1] << 8) |
                 (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;
    return val;
}

SWFParser::SWFParser() {}

SWFParser::~SWFParser() {}

bool SWFParser::parseFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        error_ = "Cannot open file: " + filename;
        return false;
    }
    
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();
    
    // Check signature
    if (data.size() < 3) {
        error_ = "File too small";
        return false;
    }
    
    std::string sig(reinterpret_cast<const char*>(data.data()), 3);
    
    // Decompress if needed
    if (sig == "CWS" || sig == "ZWS") {
        try {
            // Get header first
            document_.header = SWFHeaderParser::parse(data);
            
            // Validate fileLength
            if (document_.header.fileLength < 8) {
                error_ = "Invalid fileLength in header (too small)";
                return false;
            }
            const uint32 MAX_FILE_SIZE = 500 * 1024 * 1024;  // 500MB
            if (document_.header.fileLength > MAX_FILE_SIZE) {
                error_ = "File size exceeds maximum allowed (500MB)";
                return false;
            }
            
            // Decompress
            if (sig == "CWS") {
                // Validate compressed data size
                if (data.size() <= 8) {
                    error_ = "Compressed data is too small";
                    return false;
                }
                
                uint32 compressedSize = static_cast<uint32>(data.size()) - 8;
                uint32 uncompressedSize = document_.header.fileLength - 8;
                
                // Sanity check: compressed data should generally be smaller than uncompressed
                // unless the data is very small
                if (compressedSize > uncompressedSize * 2 && uncompressedSize > 1024) {
                    // This is suspicious but not necessarily an error
                    // Log a warning but continue
                }
                
                std::vector<uint8> decompressed = SWFHeaderParser::decompressZlib(
                    data.data() + 8, compressedSize, uncompressedSize);
                
                // Verify decompressed size matches expected
                if (decompressed.size() != uncompressedSize) {
                    int64_t diff = static_cast<int64_t>(decompressed.size()) - static_cast<int64_t>(uncompressedSize);
                    if (std::abs(diff) > 1024 && std::abs(diff) > uncompressedSize / 10) {
                        error_ = "Decompressed size mismatch: expected " + 
                                 std::to_string(uncompressedSize) + ", got " + 
                                 std::to_string(decompressed.size());
                        return false;
                    }
                    document_.header.fileLength = 8 + decompressed.size();
                }
                
                // Replace with decompressed data (keep first 8 bytes: signature, version, length)
                data.resize(8 + decompressed.size());
                memcpy(data.data() + 8, decompressed.data(), decompressed.size());
                data[0] = 'F';  // Change to uncompressed signature
            } else if (sig == "ZWS") {
                // ZWS format: signature(3) + version(1) + uncompressed_size(4) + compressed_size(4) + lzma_props(5) + compressed_data
                if (data.size() < 12) {
                    error_ = "ZWS data is too small for header";
                    return false;
                }
                
                uint32 compressedSize;
                memcpy(&compressedSize, data.data() + 8, 4);  // Compressed size at offset 8
                uint32 uncompressedSize = document_.header.fileLength - 8;
                
                // Validate compressedSize
                if (compressedSize > data.size() - 12) {
                    error_ = "Invalid compressedSize in ZWS header";
                    return false;
                }
                
                // LZMA properties are at offset 12 (5 bytes)
                std::vector<uint8> decompressed = SWFHeaderParser::decompressLZMA(
                    data.data() + 12, compressedSize + 5, uncompressedSize);
                
                // Verify decompressed size
                if (decompressed.size() != uncompressedSize) {
                    int64_t diff = static_cast<int64_t>(decompressed.size()) - static_cast<int64_t>(uncompressedSize);
                    if (std::abs(diff) > 1024 && std::abs(diff) > uncompressedSize / 10) {
                        error_ = "Decompressed size mismatch: expected " + 
                                 std::to_string(uncompressedSize) + ", got " + 
                                 std::to_string(decompressed.size());
                        return false;
                    }
                    document_.header.fileLength = 8 + decompressed.size();
                }
                
                // Replace with decompressed data
                data.resize(8 + decompressed.size());
                memcpy(data.data() + 8, decompressed.data(), decompressed.size());
                data[0] = 'F';  // Change to uncompressed signature
            }
        } catch (const std::exception& e) {
            error_ = std::string("Decompression failed: ") + e.what();
            return false;
        }
    }
    
    // Parse full header (for CWS this re-parses after decompression)
    try {
        document_.header = SWFHeaderParser::parse(data);
    } catch (const std::exception& e) {
        error_ = std::string("Header parsing failed: ") + e.what();
        return false;
    }
    
    // Parse tags
    return parseInternal(data);
}

bool SWFParser::parse(const uint8* data, size_t size) {
    std::vector<uint8> vec(data, data + size);
    return parse(vec);
}

bool SWFParser::parse(const std::vector<uint8>& data) {
    // First parse header
    if (data.size() < 8) {
        error_ = "Data too small";
        return false;
    }
    
    document_.header = SWFHeaderParser::parse(data);
    
    // Validate fileLength
    if (document_.header.fileLength < 8) {
        error_ = "Invalid fileLength in header (too small)";
        return false;
    }
    const uint32 MAX_FILE_SIZE = 500 * 1024 * 1024;  // 500MB
    if (document_.header.fileLength > MAX_FILE_SIZE) {
        error_ = "File size exceeds maximum allowed (500MB)";
        return false;
    }
    
    // If compressed, decompress
    std::vector<uint8> workData = data;
    if (document_.header.compressed) {
        if (document_.header.signature == "CWS") {
            if (data.size() <= 8) {
                error_ = "Compressed data is too small";
                return false;
            }
            
            uint32 compressedSize = static_cast<uint32>(data.size()) - 8;
            uint32 uncompressedSize = document_.header.fileLength - 8;
            
            try {
                std::vector<uint8> decompressed = SWFHeaderParser::decompressZlib(
                    data.data() + 8, compressedSize, uncompressedSize);
                
                // Verify decompressed size matches expected
                if (decompressed.size() != uncompressedSize) {
                    // Size mismatch - could be corrupted file or wrong fileLength
                    // Allow if it's within 10% tolerance for safety
                    int64_t diff = static_cast<int64_t>(decompressed.size()) - static_cast<int64_t>(uncompressedSize);
                    if (std::abs(diff) > 1024 && std::abs(diff) > uncompressedSize / 10) {
                        error_ = "Decompressed size mismatch: expected " + 
                                 std::to_string(uncompressedSize) + ", got " + 
                                 std::to_string(decompressed.size());
                        return false;
                    }
                    // Update expected fileLength to match actual
                    document_.header.fileLength = 8 + decompressed.size();
                }
                
                workData.resize(8 + decompressed.size());
                memcpy(workData.data() + 8, decompressed.data(), decompressed.size());
                workData[0] = 'F';
                
                // Re-parse header after decompression
                try {
                    document_.header = SWFHeaderParser::parse(workData);
                } catch (const std::exception& e) {
                    error_ = std::string("Header parsing failed: ") + e.what();
                    return false;
                }
            } catch (const std::exception& e) {
                error_ = std::string("Decompression failed: ") + e.what();
                return false;
            }
        } else if (document_.header.signature == "ZWS") {
            // ZWS format: signature(3) + version(1) + uncompressed_size(4) + compressed_size(4) + lzma_props(5) + compressed_data
            if (data.size() < 12) {
                error_ = "ZWS data is too small for header";
                return false;
            }
            
            uint32 compressedSize;
            memcpy(&compressedSize, data.data() + 8, 4);  // Compressed size at offset 8
            uint32 uncompressedSize = document_.header.fileLength - 8;
            
            // Validate compressedSize
            if (compressedSize > data.size() - 12) {
                error_ = "Invalid compressedSize in ZWS header";
                return false;
            }
            
            try {
                // LZMA properties are at offset 12 (5 bytes), followed by compressed data
                std::vector<uint8> decompressed = SWFHeaderParser::decompressLZMA(
                    data.data() + 12, compressedSize + 5, uncompressedSize);
                
                // Verify decompressed size
                if (decompressed.size() != uncompressedSize) {
                    int64_t diff = static_cast<int64_t>(decompressed.size()) - static_cast<int64_t>(uncompressedSize);
                    if (std::abs(diff) > 1024 && std::abs(diff) > uncompressedSize / 10) {
                        error_ = "Decompressed size mismatch: expected " + 
                                 std::to_string(uncompressedSize) + ", got " + 
                                 std::to_string(decompressed.size());
                        return false;
                    }
                    document_.header.fileLength = 8 + decompressed.size();
                }
                
                workData.resize(8 + decompressed.size());
                memcpy(workData.data() + 8, decompressed.data(), decompressed.size());
                workData[0] = 'F';
                
                // Re-parse header after decompression
                try {
                    document_.header = SWFHeaderParser::parse(workData);
                } catch (const std::exception& e) {
                    error_ = std::string("Header parsing failed: ") + e.what();
                    return false;
                }
            } catch (const std::exception& e) {
                error_ = std::string("Decompression failed: ") + e.what();
                return false;
            }
        }
    }
    
    return parseInternal(workData);
}

bool SWFParser::parseInternal(const std::vector<uint8>& data) {
    // Start parsing after header (including RECT, frame rate, frame count)
    size_t pos = document_.header.headerSize;
    
    // Align to byte boundary
    if (pos < data.size()) {
        // Header parsing already aligned
    }
    
    // Track current frame for display list building
    uint16 currentFrameNum = 0;
    Frame currentFrame;
    currentFrame.frameNumber = currentFrameNum;
    
    // Parse all tags
    while (pos < data.size() - 2) {
        if (pos + 2 > data.size()) break;
        
        // Get tag info without consuming position
        uint16 headerWord = data[pos] | (data[pos + 1] << 8);
        uint16 tagCode = headerWord >> 6;
        
        // Handle SHOW_FRAME specially
        if (tagCode == 1) { // SHOW_FRAME
            // Save current frame and start new one
            if (!currentFrame.displayListOps.empty() || !currentFrame.label.empty()) {
                document_.frames.push_back(std::move(currentFrame));
            }
            currentFrameNum++;
            currentFrame = Frame();
            currentFrame.frameNumber = currentFrameNum;
        }
        
        parseTag(data.data(), data.size(), pos, currentFrame);
    }
    
    // Don't forget the last frame
    if (!currentFrame.displayListOps.empty() || !currentFrame.label.empty()) {
        document_.frames.push_back(std::move(currentFrame));
    }
    
    valid_ = true;
    return true;
}

void SWFParser::parseTag(const uint8* data, size_t dataSize, size_t& pos, Frame& currentFrame) {
    // Read tag header (little-endian)
    uint16 headerWord = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    
    uint16 tagCode = headerWord >> 6;
    uint32 tagLength = headerWord & 0x3F;
    
    // If length is 0x3F, read extended length
    if (tagLength == 0x3F) {
        if (pos + 4 > dataSize) {
            pos = dataSize;
            return;
        }
        tagLength = data[pos] | (data[pos + 1] << 8) | 
                   (data[pos + 2] << 16) | (data[pos + 3] << 24);
        pos += 4;
    }
    
    // Check if we have enough data
    if (pos + tagLength > dataSize) {
        std::cerr << "Warning: Tag " << tagCode << " extends past end of file" << std::endl;
        tagLength = static_cast<uint32>(dataSize - pos);
    }
    
    // Parse based on tag type
    switch (tagCode) {
        case 0: // END
            break;
            
        case 1: // SHOW_FRAME
            break;
            
        case 9: // SET_BACKGROUND_COLOR
            parseSetBackgroundColor(data + pos, tagLength);
            break;
            
        case 2: // DEFINE_SHAPE
        case 22: // DEFINE_SHAPE2
        case 32: // DEFINE_SHAPE3
            parseDefineShape3(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 6: // DEFINE_BITS
        case 21: // DEFINE_BITS_JPEG2
        case 35: // DEFINE_BITS_JPEG3
            parseDefineBits(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 20: // DEFINE_BITS_LOSSLESS
        case 36: // DEFINE_BITS_LOSSLESS2
            parseDefineBitsLossless(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 10: // DEFINE_FONT
        case 48: // DEFINE_FONT2
            parseDefineFont2(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 75: // DEFINE_FONT3
            parseDefineFont3(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 11: // DEFINE_TEXT
        case 33: // DEFINE_TEXT2
            parseDefineText(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 37: // DEFINE_EDIT_TEXT
            parseDefineEditText(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 12: // DO_ACTION
        case 59: // DO_INIT_ACTION
            parseDoAction(data, dataSize, pos, tagLength);
            break;
            
        case 82: // DO_ABC
            parseDoABC(data, dataSize, pos, tagLength);
            break;
            
        case 56: // EXPORT_ASSETS
            parseExportAssets(data + pos, tagLength);
            break;
            
        case 76: // SYMBOL_CLASS
            parseSymbolClass(data + pos, tagLength);
            break;
            
        case 4:  // PLACE_OBJECT
        case 26: // PLACE_OBJECT2
        case 27: // REMOVE_OBJECT  
        case 28: // REMOVE_OBJECT2
        case 70: // PLACE_OBJECT3
            parsePlaceObject(data, dataSize, pos, tagCode, tagLength, currentFrame);
            break;
            
        case 38: // FRAME_LABEL
            parseFrameLabel(data + pos, tagLength, currentFrame);
            break;
            
        case 14: // DEFINE_SOUND
            parseDefineSound(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 18: // SOUND_STREAM_HEAD
        case 45: // SOUND_STREAM_HEAD2
            parseSoundStreamHead(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 19: // SOUND_STREAM_BLOCK
            parseSoundStreamBlock(data, dataSize, pos, tagLength);
            break;
            
        case 15: // START_SOUND
            parseStartSound(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 7:  // DEFINE_BUTTON
        case 34: // DEFINE_BUTTON2
            parseDefineButton(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 17: // DEFINE_BUTTON_SOUND
            parseDefineButtonSound(data, dataSize, pos, tagLength);
            break;
            
        case 23: // DEFINE_BUTTON_CXFORM
            parseDefineButtonCxform(data, dataSize, pos, tagLength);
            break;
            
        case 46: // DEFINE_MORPH_SHAPE
            parseDefineMorphShape(data, dataSize, pos, tagLength);
            break;
            
        case 39: // DEFINE_SPRITE (MovieClip)
            parseDefineSprite(data, dataSize, pos, tagCode, tagLength);
            break;
            
        case 60: // DEFINE_VIDEO_STREAM
            parseDefineVideoStream(data, dataSize, pos, tagLength);
            break;
            
        case 61: // VIDEO_FRAME
            parseVideoFrame(data, dataSize, pos, tagLength);
            break;
            
        case 69: // FILE_ATTRIBUTES
            parseFileAttributes(data + pos, tagLength);
            break;
            
        default:
            skipTag(data, dataSize, pos, tagLength);
            break;
    }
    
    pos += tagLength;
}

void SWFParser::parseSetBackgroundColor(const uint8* data, size_t size) {
    if (size < 3) return;
    
    document_.backgroundColor = RGBA(data[0], data[1], data[2], 255);
    document_.hasBackgroundColor = true;
}

void SWFParser::parseDefineShape3(const uint8* data, size_t size, size_t pos, 
                                   uint16 code, uint32 length) {
    if (pos + 4 > size) return;
    
    auto shapeTag = std::make_unique<ShapeTag>(code, length, pos);
    
    // Read shape ID (little-endian)
    shapeTag->shapeId = data[pos] | (data[pos + 1] << 8);
    
    // Read bounds
    BitStream bs(data + pos + 2, size - pos - 2);
    int nBits = static_cast<int>(bs.readUB(5));
    shapeTag->bounds.xMin = bs.readSB(nBits);
    shapeTag->bounds.xMax = bs.readSB(nBits);
    shapeTag->bounds.yMin = bs.readSB(nBits);
    shapeTag->bounds.yMax = bs.readSB(nBits);
    bs.alignByte();
    
    // Read styles
    int numFill = static_cast<int>(bs.readUB(5));
    int numLine = static_cast<int>(bs.readUB(5));
    
    shapeTag->fillStyles = parseFillStyles(bs, numFill, code >= 32);
    shapeTag->lineStyles = parseLineStyles(bs, numLine, code >= 32);
    
    // Read shape records
    shapeTag->records = parseShapeRecords(bs, code >= 32, false);
    
    // Add to document
    document_.characters[shapeTag->shapeId] = std::move(shapeTag);
    
    SWFTag tag(std::move(shapeTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineBits(const uint8* data, size_t size, size_t pos,
                                 uint16 code, uint32 length) {
    if (pos + 2 > size) return;
    
    auto imageTag = std::make_unique<ImageTag>(code, length, pos);
    imageTag->imageId = (data[pos] << 8) | data[pos + 1];
    
    if (code == 6 || code == 21) {
        // JPEG data (without JPEG tables)
        imageTag->format = ImageTag::Format::JPEG;
        if (pos + 2 < size) {
            imageTag->imageData.assign(data + pos + 2, data + size);
        }
    } else if (code == 35) {
        // JPEG with alpha
        imageTag->format = ImageTag::Format::JPEG;
        // Has alpha data after image
        if (pos + 2 < size) {
            imageTag->imageData.assign(data + pos + 2, data + size);
        }
    }
    
    document_.characters[imageTag->imageId] = std::move(imageTag);
    
    SWFTag tag(std::move(imageTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineBitsLossless(const uint8* data, size_t size, size_t pos,
                                         uint16 code, uint32 length) {
    if (pos + 3 > size) return;
    
    auto imageTag = std::make_unique<ImageTag>(code, length, pos);
    imageTag->imageId = (data[pos] << 8) | data[pos + 1];
    
    uint8 format = data[pos + 2];
    
    if (code == 20) {
        // Lossless (no alpha)
        imageTag->format = ImageTag::Format::LOSSLESS;
    } else {
        // Lossless2 (has alpha)
        imageTag->format = ImageTag::Format::LOSSLESS_ALPHA;
    }
    
    if (pos + 3 < size) {
        imageTag->imageData.assign(data + pos + 3, data + size);
    }
    
    document_.characters[imageTag->imageId] = std::move(imageTag);
    
    SWFTag tag(std::move(imageTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineFont2(const uint8* data, size_t size, size_t pos,
                                  uint16 code, uint32 length) {
    if (pos + 2 > size) return;
    
    auto fontTag = std::make_unique<FontTag>(code, length, pos);
    fontTag->fontId = (data[pos] << 8) | data[pos + 1];
    
    BitStream bs(data + pos + 2, size - pos - 2);
    
    // Font info flags
    uint8 flags = static_cast<uint8>(bs.readUB(8));
    fontTag->isFont3 = (flags & 0x80) != 0;
    bool hasLayout = (flags & 0x04) != 0;
    bool isShiftJIS = (flags & 0x08) != 0;
    bool isSmallText = (flags & 0x20) != 0;
    bool usesANSI = (flags & 0x10) != 0;
    bool isItalic = (flags & 0x40) != 0;
    bool isBold = (flags & 0x01) != 0;
    
    // Skip language code
    bs.readUB(8);
    
    // Font name
    uint8 nameLen = static_cast<uint8>(bs.readUB(8));
    std::string fontName;
    for (int i = 0; i < nameLen && bs.hasMoreBits(); i++) {
        fontName += static_cast<char>(bs.readUB(8));
    }
    fontTag->fontName = fontName;
    
    // Number of glyphs
    uint16 numGlyphs = static_cast<uint16>(bs.readUB(16));
    
    // Offset table
    std::vector<uint16> offsets;
    for (int i = 0; i < numGlyphs; i++) {
        offsets.push_back(static_cast<uint16>(bs.readUB(16)));
    }
    
    // Code table
    if (flags & 0x08) {  // Has code table
        for (int i = 0; i < numGlyphs; i++) {
            fontTag->codeTable.push_back(static_cast<uint16>(bs.readUB(16)));
        }
    }
    
    // Glyph shapes - simplified, just store raw data
    // (In a real implementation, would parse each glyph)
    for (int i = 0; i < numGlyphs; i++) {
        GlyphEntry glyph;
        fontTag->glyphs.push_back(glyph);
    }
    
    // Layout
    if (hasLayout) {
        fontTag->hasLayout = true;
        fontTag->ascent = static_cast<int16>(bs.readUB(16));
        fontTag->descent = static_cast<int16>(bs.readUB(16));
        fontTag->leading = static_cast<int16>(bs.readUB(16));
        
        // Advance table
        for (int i = 0; i < numGlyphs; i++) {
            fontTag->advanceTable.push_back(static_cast<int16>(bs.readUB(16)));
        }
        
        // Bounds table
        for (int i = 0; i < numGlyphs; i++) {
            Rect bounds;
            int nBits = static_cast<int>(bs.readUB(5));
            bounds.xMin = bs.readSB(nBits);
            bounds.xMax = bs.readSB(nBits);
            bounds.yMin = bs.readSB(nBits);
            bounds.yMax = bs.readSB(nBits);
            fontTag->boundsTable.push_back(bounds);
        }
        
        // Kerning
        uint16 numKern = static_cast<uint16>(bs.readUB(16));
        for (int i = 0; i < numKern; i++) {
            KerningRecord kr;
            kr.code1 = static_cast<uint16>(bs.readUB(16));
            kr.code2 = static_cast<uint16>(bs.readUB(16));
            kr.adjustment = static_cast<int16>(bs.readUB(16));
            fontTag->kerningTable.push_back(kr);
        }
    }
    
    document_.characters[fontTag->fontId] = std::move(fontTag);
    
    SWFTag tag(std::move(fontTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineFont3(const uint8* data, size_t size, size_t pos,
                                   uint16 code, uint32 length) {
    // Similar to DefineFont2 but with full Unicode support
    parseDefineFont2(data, size, pos, code, length);
}

void SWFParser::parseDefineText(const uint8* data, size_t size, size_t pos, 
                                   uint16 code, uint32 length) {
    if (pos + 2 > size) return;
    
    auto textTag = std::make_unique<TextTag>(code, length, pos);
    textTag->textId = data[pos] | (data[pos + 1] << 8);
    
    // Read bounds
    BitStream bs(data + pos + 2, size - pos - 2);
    int nBits = static_cast<int>(bs.readUB(5));
    textTag->bounds.xMin = bs.readSB(nBits);
    textTag->bounds.xMax = bs.readSB(nBits);
    textTag->bounds.yMin = bs.readSB(nBits);
    textTag->bounds.yMax = bs.readSB(nBits);
    bs.alignByte();
    
    // Parse matrix
    textTag->records.push_back(TextRecord()); // Current record
    TextRecord* currentRecord = &textTag->records.back();
    
    // Matrix
    bool hasScale = bs.readUB(1);
    if (hasScale) {
        int scaleBits = static_cast<int>(bs.readUB(5));
        currentRecord->fontMatrix.scaleX = bs.readSB(scaleBits) / 65536.0f;
        currentRecord->fontMatrix.scaleY = bs.readSB(scaleBits) / 65536.0f;
        currentRecord->fontMatrix.hasScale = true;
    }
    
    bool hasRotate = bs.readUB(1);
    if (hasRotate) {
        int rotateBits = static_cast<int>(bs.readUB(5));
        currentRecord->fontMatrix.rotate0 = bs.readSB(rotateBits) / 65536.0f;
        currentRecord->fontMatrix.rotate1 = bs.readSB(rotateBits) / 65536.0f;
        currentRecord->fontMatrix.hasRotate = true;
    }
    
    int translateBits = static_cast<int>(bs.readUB(5));
    currentRecord->fontMatrix.translateX = bs.readSB(translateBits);
    currentRecord->fontMatrix.translateY = bs.readSB(translateBits);
    bs.alignByte();
    
    // Glyph bits and advance bits
    uint8 glyphBits = static_cast<uint8>(bs.readUB(8));
    uint8 advanceBits = static_cast<uint8>(bs.readUB(8));
    
    // Text records
    while (bs.hasMoreBits()) {
        uint8 flags = static_cast<uint8>(bs.readUB(8));
        
        if (flags == 0) {
            break; // End of text records
        }
        
        // Check if it's a style change record
        if ((flags & 0x80) == 0) {
            // This is a style change record
            TextRecord newRecord = *currentRecord; // Inherit from previous
            
            bool hasFont = (flags >> 3) & 0x01;
            bool hasColor = (flags >> 2) & 0x01;
            bool hasYOffset = (flags >> 1) & 0x01;
            bool hasXOffset = (flags >> 0) & 0x01;
            
            if (hasFont) {
                newRecord.fontId = static_cast<uint16>(bs.readUB(16));
            }
            if (hasColor) {
                newRecord.textColor.r = static_cast<uint8>(bs.readUB(8));
                newRecord.textColor.g = static_cast<uint8>(bs.readUB(8));
                newRecord.textColor.b = static_cast<uint8>(bs.readUB(8));
                if (code == 34) { // DefineText2 has alpha
                    newRecord.textColor.a = static_cast<uint8>(bs.readUB(8));
                } else {
                    newRecord.textColor.a = 255;
                }
            }
            if (hasXOffset) {
                newRecord.xOffset = static_cast<int32>(bs.readSB(16));
            }
            if (hasYOffset) {
                newRecord.yOffset = static_cast<int32>(bs.readSB(16));
            }
            if (hasFont) {
                newRecord.fontHeight = static_cast<uint16>(bs.readUB(16));
            }
            
            textTag->records.push_back(newRecord);
            currentRecord = &textTag->records.back();
        } else {
            // Glyph record - read number of glyphs
            uint8 glyphCount = flags & 0x7F;

            // Parse each glyph entry
            for (int i = 0; i < glyphCount && bs.hasMoreBits(); i++) {
                TextGlyphEntry entry;
                entry.glyphIndex = static_cast<uint32>(bs.readUB(glyphBits));
                entry.glyphAdvance = bs.readSB(advanceBits);
                currentRecord->glyphs.push_back(entry);
            }
        }
    }
    
    // Remove empty initial record if no text was parsed
    if (textTag->records.size() == 1 && textTag->records[0].fontId == 0) {
        textTag->records.clear();
    }
    
    document_.characters[textTag->textId] = std::make_unique<TagBase>(*textTag);
    
    SWFTag tag(std::move(textTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineEditText(const uint8* data, size_t size, size_t& pos,
                                      uint16 code, uint32 length) {
    if (pos + 2 > size) return;

    auto editTag = std::make_unique<EditTextTag>(code, length, pos);

    // Character ID (UI16)
    editTag->characterId = readU16(data, pos);

    // Bounds (RECT)
    if (pos + 1 > size) return;
    BitStream bs(data, pos * 8);
    uint8_t nBits = static_cast<uint8_t>(bs.readUB(5));
    editTag->bounds.xMin = bs.readSB(nBits);
    editTag->bounds.xMax = bs.readSB(nBits);
    editTag->bounds.yMin = bs.readSB(nBits);
    editTag->bounds.yMax = bs.readSB(nBits);
    pos = (bs.bitPos() + 7) / 8;

    if (pos >= size) return;

    // Flags
    uint8_t flags1 = data[pos++];
    bool hasText = (flags1 >> 7) & 1;
    editTag->wordWrap = (flags1 >> 6) & 1;
    editTag->multiline = (flags1 >> 5) & 1;
    editTag->password = (flags1 >> 4) & 1;
    editTag->readOnly = (flags1 >> 3) & 1;
    bool hasTextColor = (flags1 >> 2) & 1;
    bool hasMaxLength = (flags1 >> 1) & 1;
    bool hasFont = flags1 & 1;

    if (pos >= size) return;
    uint8_t flags2 = data[pos++];
    bool hasFontClass = (flags2 >> 7) & 1;
    editTag->autoSize = (flags2 >> 6) & 1;
    bool hasLayout = (flags2 >> 5) & 1;
    editTag->noSelect = (flags2 >> 4) & 1;
    editTag->border = (flags2 >> 3) & 1;
    // WasStatic bit is reserved
    editTag->html = (flags2 >> 1) & 1;
    editTag->useOutlines = flags2 & 1;

    // Font ID
    if (hasFont) {
        if (pos + 2 > size) return;
        editTag->fontId = readU16(data, pos);
    }

    // Font class name (if has font class)
    if (hasFontClass) {
        // Skip font class string
        while (pos < size && data[pos] != 0) pos++;
        if (pos < size) pos++;  // Skip null terminator
    }

    // Font height
    if (hasFont) {
        if (pos + 2 > size) return;
        editTag->fontHeight = readU16(data, pos);
    }

    // Text color
    if (hasTextColor) {
        if (pos + 4 > size) return;
        editTag->textColor.r = data[pos++];
        editTag->textColor.g = data[pos++];
        editTag->textColor.b = data[pos++];
        editTag->textColor.a = data[pos++];
    }

    // Max length
    if (hasMaxLength) {
        if (pos + 2 > size) return;
        editTag->maxLength = readU16(data, pos);
    }

    // Layout
    if (hasLayout) {
        if (pos + 9 > size) return;
        editTag->align = data[pos++];
        editTag->leftMargin = readU16(data, pos);
        editTag->rightMargin = readU16(data, pos);
        editTag->indent = readU16(data, pos);
        editTag->leading = static_cast<int16>(readU16(data, pos));
    }

    // Variable name
    editTag->variableName = readString(data, pos, size);

    // Initial text
    if (hasText) {
        editTag->initialText = readString(data, pos, size);
    }

    SWFTag tag(std::move(editTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDoAction(const uint8* data, size_t size, size_t pos,
                                uint32 length) {
    auto actionTag = std::make_unique<ActionTag>(12, length, pos);
    actionTag->actionData.assign(data + pos, data + pos + length);
    
    SWFTag tag(std::move(actionTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDoABC(const uint8* data, size_t size, size_t pos,
                            uint32 length) {
    if (pos + 4 > size) return;
    
    auto abcTag = std::make_unique<ABCFileTag>(82, length, pos);
    
    // Flags (lazy_init, etc.)
    abcTag->flags = (data[pos] << 24) | (data[pos + 1] << 16) | 
                   (data[pos + 2] << 8) | data[pos + 3];
    
    // Name (optional)
    if (pos + 8 < size) {
        uint32 nameLen = (data[pos + 4] << 24) | (data[pos + 5] << 16) |
                        (data[pos + 6] << 8) | data[pos + 7];
        if (nameLen > 0 && pos + 8 + nameLen <= size) {
            abcTag->name = std::string(reinterpret_cast<const char*>(data + pos + 8), nameLen);
        }
    }
    
    // ABC data
    size_t abcDataStart = pos + 4; // Skip flags
    if (!abcTag->name.empty()) {
        abcDataStart += 4 + abcTag->name.length(); // Skip name length + name
    }
    
    if (abcDataStart < size) {
        abcTag->abcData.assign(data + abcDataStart, data + size);
    }
    
    document_.abcFiles.push_back(std::move(abcTag));
    
    SWFTag tag(std::move(document_.abcFiles.back()));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseExportAssets(const uint8* data, size_t size) {
    if (size < 2) return;
    
    uint16 count = data[0] | (data[1] << 8);  // little-endian
    size_t pos = 2;
    
    for (uint16 i = 0; i < count && pos + 2 < size; i++) {
        uint16 charId = data[pos] | (data[pos + 1] << 8);  // little-endian
        pos += 2;
        
        // Read name
        std::string name;
        while (pos < size && data[pos] != 0) {
            name += static_cast<char>(data[pos]);
            pos++;
        }
        pos++; // Skip null
        
        if (!name.empty()) {
            document_.exports.emplace_back(charId, name);
            document_.exportNameToId[name] = charId;
        }
    }
}

void SWFParser::parseSymbolClass(const uint8* data, size_t size) {
    if (size < 2) return;
    
    uint16 numSymbols = (data[0] << 8) | data[1];
    size_t pos = 2;
    
    for (uint16 i = 0; i < numSymbols && pos + 3 < size; i++) {
        uint16 charId = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        
        // Read name
        std::string name;
        while (pos < size && data[pos] != 0) {
            name += static_cast<char>(data[pos]);
            pos++;
        }
        pos++; // Skip null
        
        if (!name.empty()) {
            document_.symbolClass[charId] = name;
        }
    }
}

void SWFParser::parsePlaceObject(const uint8* data, size_t size, size_t pos,
                                   uint16 code, uint32 length, Frame& currentFrame) {
    // Handle different place/remove object tags
    switch (code) {
        case 4:  // PLACE_OBJECT (PlaceObject)
            parsePlaceObject1(data, size, pos, currentFrame);
            break;
            
        case 26: // PLACE_OBJECT2
            parsePlaceObject2(data, size, pos, currentFrame);
            break;
            
        case 70: // PLACE_OBJECT3
            parsePlaceObject3(data, size, pos, currentFrame);
            break;
            
        case 5:  // REMOVE_OBJECT
        case 28: // REMOVE_OBJECT2
            parseRemoveObject(data, size, pos, code, currentFrame);
            break;
    }
}

void SWFParser::parsePlaceObject1(const uint8* data, size_t size, size_t pos, Frame& currentFrame) {
    // PlaceObject (tag 4): CharacterId, Depth, Matrix, ColorTransform
    if (pos + 6 > size) return;
    
    DisplayItem item;
    item.type = DisplayItem::Type::PLACE;
    
    // Character ID (2 bytes)
    item.characterId = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    
    // Depth (2 bytes)
    item.depth = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    
    // Matrix
    BitStream bs(data + pos, size - pos);
    item.matrix = parseMatrix(bs);
    
    // Color transform (optional in original, but usually present)
    // Skip for now as PlaceObject rarely has cxform in simple files
    
    // Remove any existing item at this depth
    currentFrame.removeByDepth(item.depth);
    currentFrame.displayListOps.push_back(item);
}

void SWFParser::parsePlaceObject2(const uint8* data, size_t size, size_t pos, Frame& currentFrame) {
    // PlaceObject2 (tag 26): Has more flags and optional fields
    if (pos >= size) return;
    
    DisplayItem item;
    item.type = DisplayItem::Type::PLACE;
    
    // Flags (1 byte)
    uint8 flags = data[pos++];
    bool hasClipActions = (flags >> 7) & 1;
    bool hasClipDepth = (flags >> 6) & 1;
    bool hasName = (flags >> 5) & 1;
    bool hasRatio = (flags >> 4) & 1;
    bool hasColorTransform = (flags >> 3) & 1;
    bool hasMatrix = (flags >> 2) & 1;
    bool hasCharacter = (flags >> 1) & 1;
    bool isMove = flags & 1;
    
    // Depth (2 bytes)
    if (pos + 2 > size) return;
    item.depth = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    
    // Character ID (if present)
    if (hasCharacter) {
        if (pos + 2 > size) return;
        item.characterId = (data[pos] << 8) | data[pos + 1];
        pos += 2;
    }
    
    // Matrix (if present)
    if (hasMatrix) {
        BitStream bs(data + pos, size - pos);
        item.matrix = parseMatrix(bs);
        pos += bs.bytePos();
    }
    
    // Color transform (if present)
    if (hasColorTransform) {
        BitStream bs(data + pos, size - pos);
        // Parse CXFORMWITHALPHA
        bool hasAddTerms = bs.readUB(1);
        bool hasMultTerms = bs.readUB(1);
        int nbits = static_cast<int>(bs.readUB(4));
        
        if (hasMultTerms) {
            item.cxform.mulR = bs.readSB(nbits) / 256.0f;
            item.cxform.mulG = bs.readSB(nbits) / 256.0f;
            item.cxform.mulB = bs.readSB(nbits) / 256.0f;
            if (hasAddTerms) {
                item.cxform.addR = bs.readSB(nbits);
                item.cxform.addG = bs.readSB(nbits);
                item.cxform.addB = bs.readSB(nbits);
                item.cxform.hasAdd = true;
            }
        }
        if (hasAddTerms && !hasMultTerms) {
            item.cxform.addR = bs.readSB(nbits);
            item.cxform.addG = bs.readSB(nbits);
            item.cxform.addB = bs.readSB(nbits);
        }
        pos += bs.bytePos();
    }
    
    // Ratio/morph position (if present)
    if (hasRatio) {
        if (pos + 2 > size) return;
        item.ratio = data[pos + 1]; // Just store lower byte for now
        pos += 2;
    }
    
    // Instance name (if present)
    if (hasName) {
        while (pos < size && data[pos] != 0) {
            item.instanceName += static_cast<char>(data[pos]);
            pos++;
        }
        if (pos < size) pos++; // Skip null terminator
    }
    
    // Clip depth (if present)
    if (hasClipDepth) {
        if (pos + 2 > size) return;
        item.clipDepth = (data[pos] << 8) | data[pos + 1];
        pos += 2;
    }
    
    // Clip actions
    if (hasClipActions) {
        parseClipActions(data, size, pos, item.clipActions, false);
        item.hasClipActions = !item.clipActions.empty();
    }

    // Handle move flag
    if (isMove && !hasCharacter) {
        item.type = DisplayItem::Type::MOVE;
    }
    
    // Remove existing item at this depth and add new one
    currentFrame.removeByDepth(item.depth);
    currentFrame.displayListOps.push_back(item);
}

void SWFParser::parsePlaceObject3(const uint8* data, size_t size, size_t pos, Frame& currentFrame) {
    // PlaceObject3 (tag 70): Similar to PlaceObject2 with additional fields
    if (pos + 2 > size) return;
    
    DisplayItem item;
    item.type = DisplayItem::Type::PLACE;
    
    // Flags (2 bytes)
    uint8 flags = data[pos++];
    uint8 flags2 = data[pos++];
    
    bool hasClipActions = (flags >> 7) & 1;
    bool hasClipDepth = (flags >> 6) & 1;
    bool hasName = (flags >> 5) & 1;
    bool hasRatio = (flags >> 4) & 1;
    bool hasColorTransform = (flags >> 3) & 1;
    bool hasMatrix = (flags >> 2) & 1;
    bool hasCharacter = (flags >> 1) & 1;
    bool isMove = flags & 1;
    
    bool hasImage = (flags2 >> 3) & 1;
    bool hasClassName = (flags2 >> 2) & 1;
    bool hasCacheAsBitmap = (flags2 >> 1) & 1;
    bool hasBlendMode = flags2 & 1;
    bool hasFilterList = (flags2 >> 4) & 1;
    
    // Depth (2 bytes)
    if (pos + 2 > size) return;
    item.depth = (data[pos] << 8) | data[pos + 1];
    pos += 2;
    
    // Class name (if present)
    if (hasClassName || (hasImage && hasCharacter)) {
        while (pos < size && data[pos] != 0) {
            pos++;
        }
        if (pos < size) pos++; // Skip null
    }
    
    // Character ID (if present)
    if (hasCharacter) {
        if (pos + 2 > size) return;
        item.characterId = (data[pos] << 8) | data[pos + 1];
        pos += 2;
    }
    
    // Matrix (if present)
    if (hasMatrix) {
        BitStream bs(data + pos, size - pos);
        item.matrix = parseMatrix(bs);
        pos += bs.bytePos();
    }
    
    // Color transform (if present)
    if (hasColorTransform) {
        BitStream bs(data + pos, size - pos);
        bool hasAddTerms = bs.readUB(1);
        bool hasMultTerms = bs.readUB(1);
        int nbits = static_cast<int>(bs.readUB(4));
        
        if (hasMultTerms) {
            item.cxform.mulR = bs.readSB(nbits) / 256.0f;
            item.cxform.mulG = bs.readSB(nbits) / 256.0f;
            item.cxform.mulB = bs.readSB(nbits) / 256.0f;
        }
        if (hasAddTerms) {
            item.cxform.addR = bs.readSB(nbits);
            item.cxform.addG = bs.readSB(nbits);
            item.cxform.addB = bs.readSB(nbits);
        }
        pos += bs.bytePos();
    }
    
    // Ratio (if present)
    if (hasRatio) {
        if (pos + 2 > size) return;
        item.ratio = data[pos + 1];
        pos += 2;
    }
    
    // Instance name (if present)
    if (hasName) {
        while (pos < size && data[pos] != 0) {
            item.instanceName += static_cast<char>(data[pos]);
            pos++;
        }
        if (pos < size) pos++;
    }
    
    // Clip depth (if present)
    if (hasClipDepth) {
        if (pos + 2 > size) return;
        item.clipDepth = (data[pos] << 8) | data[pos + 1];
        pos += 2;
    }
    
    // Filter list
    if (hasFilterList) {
        if (pos < size) {
            uint8 filterCount = data[pos++];
            for (int i = 0; i < filterCount && pos < size; i++) {
                uint8 filterId = data[pos++];
                
                switch (filterId) {
                    case 0: { // DropShadow
                        if (pos + 23 > size) break;
                        DropShadowFilter ds;
                        RGBA color;
                        color.r = data[pos++];
                        color.g = data[pos++];
                        color.b = data[pos++];
                        color.a = data[pos++];
                        ds.color = color;
                        ds.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        ds.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        ds.angle = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        ds.distance = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        ds.strength = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 flags = data[pos++];
                        ds.inner = (flags >> 7) & 1;
                        ds.knockout = (flags >> 6) & 1;
                        ds.compositeSource = (flags >> 5) & 1;
                        ds.passes = flags & 0x1F;
                        item.filters.emplace_back(ds);
                        break;
                    }
                    case 1: { // Blur
                        if (pos + 9 > size) break;
                        BlurFilter blur;
                        blur.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        blur.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 passes = data[pos++];
                        blur.passes = (passes >> 3) & 0x1F;
                        item.filters.emplace_back(blur);
                        break;
                    }
                    case 2: { // Glow
                        if (pos + 23 > size) break;
                        GlowFilter glow;
                        RGBA color;
                        color.r = data[pos++];
                        color.g = data[pos++];
                        color.b = data[pos++];
                        color.a = data[pos++];
                        glow.color = color;
                        glow.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        glow.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        glow.strength = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 flags = data[pos++];
                        glow.inner = (flags >> 7) & 1;
                        glow.knockout = (flags >> 6) & 1;
                        glow.compositeSource = (flags >> 5) & 1;
                        glow.passes = flags & 0x1F;
                        item.filters.emplace_back(glow);
                        break;
                    }
                    case 3: { // Bevel
                        if (pos + 29 > size) break;
                        BevelFilter bevel;
                        bevel.shadowColor.r = data[pos++];
                        bevel.shadowColor.g = data[pos++];
                        bevel.shadowColor.b = data[pos++];
                        bevel.shadowColor.a = data[pos++];
                        bevel.highlightColor.r = data[pos++];
                        bevel.highlightColor.g = data[pos++];
                        bevel.highlightColor.b = data[pos++];
                        bevel.highlightColor.a = data[pos++];
                        bevel.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        bevel.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        bevel.angle = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        bevel.distance = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        bevel.strength = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 bevelFlags = data[pos++];
                        bevel.inner = (bevelFlags >> 7) & 1;
                        bevel.knockout = (bevelFlags >> 6) & 1;
                        bevel.compositeSource = (bevelFlags >> 5) & 1;
                        bevel.onTop = (bevelFlags >> 4) & 1;
                        bevel.passes = bevelFlags & 0x0F;
                        item.filters.emplace_back(bevel);
                        break;
                    }
                    case 4: { // GradientGlow
                        if (pos + 11 > size) break;
                        GradientGlowFilter gg;
                        gg.numColors = data[pos++];
                        if (pos + gg.numColors * 5 + 22 > size) break;
                        for (uint8 k = 0; k < gg.numColors; k++) {
                            RGBA c;
                            c.r = data[pos++];
                            c.g = data[pos++];
                            c.b = data[pos++];
                            c.a = data[pos++];
                            gg.colors.push_back(c);
                            gg.ratios.push_back(data[pos++]);
                        }
                        gg.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gg.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gg.angle = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gg.distance = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gg.strength = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 ggFlags = data[pos++];
                        gg.inner = (ggFlags >> 7) & 1;
                        gg.knockout = (ggFlags >> 6) & 1;
                        gg.compositeSource = (ggFlags >> 5) & 1;
                        gg.passes = ggFlags & 0x0F;
                        item.filters.emplace_back(gg);
                        break;
                    }
                    case 5: { // Convolution
                        if (pos + 9 > size) break;
                        ConvolutionFilter conv;
                        conv.matrixX = data[pos++];
                        conv.matrixY = data[pos++];
                        uint32 matrixSize = conv.matrixX * conv.matrixY;
                        if (pos + matrixSize * 4 + 17 > size) break;
                        conv.divisor = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        conv.bias = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        for (uint32 k = 0; k < matrixSize; k++) {
                            conv.matrix.push_back(*reinterpret_cast<const float*>(data + pos)); pos += 4;
                        }
                        conv.defaultColor.r = data[pos++];
                        conv.defaultColor.g = data[pos++];
                        conv.defaultColor.b = data[pos++];
                        conv.defaultColor.a = data[pos++];
                        uint8 convFlags = data[pos++];
                        conv.clamp = (convFlags >> 1) & 1;
                        conv.preserveAlpha = convFlags & 1;
                        item.filters.emplace_back(conv);
                        break;
                    }
                    case 6: { // ColorMatrix
                        if (pos + 80 > size) break;
                        ColorMatrixFilter cm;
                        for (int j = 0; j < 20; j++) {
                            cm.matrix[j] = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        }
                        item.filters.emplace_back(cm);
                        break;
                    }
                    case 7: { // GradientBevel
                        if (pos + 11 > size) break;
                        GradientBevelFilter gb;
                        gb.numColors = data[pos++];
                        if (pos + gb.numColors * 5 + 22 > size) break;
                        for (uint8 k = 0; k < gb.numColors; k++) {
                            RGBA c;
                            c.r = data[pos++];
                            c.g = data[pos++];
                            c.b = data[pos++];
                            c.a = data[pos++];
                            gb.colors.push_back(c);
                            gb.ratios.push_back(data[pos++]);
                        }
                        gb.blurX = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gb.blurY = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gb.angle = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gb.distance = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        gb.strength = *reinterpret_cast<const float*>(data + pos); pos += 4;
                        uint8 gbFlags = data[pos++];
                        gb.inner = (gbFlags >> 7) & 1;
                        gb.knockout = (gbFlags >> 6) & 1;
                        gb.compositeSource = (gbFlags >> 5) & 1;
                        gb.onTop = (gbFlags >> 4) & 1;
                        gb.passes = gbFlags & 0x0F;
                        item.filters.emplace_back(gb);
                        break;
                    }
                    default:
                        pos++;
                        break;
                }
            }
        }
    }
    
    if (flags & 0x20) {
        item.blendMode = static_cast<uint8>(data[pos++]);
    }
    
    // Cache as bitmap (just a flag, no data)
    (void)hasCacheAsBitmap;

    // Clip actions
    if (hasClipActions) {
        parseClipActions(data, size, pos, item.clipActions, true);
        item.hasClipActions = !item.clipActions.empty();
    }

    // Handle move flag
    if (isMove && !hasCharacter) {
        item.type = DisplayItem::Type::MOVE;
    }
    
    // Remove existing item at this depth and add new one
    currentFrame.removeByDepth(item.depth);
    currentFrame.displayListOps.push_back(item);
}

void SWFParser::parseRemoveObject(const uint8* data, size_t size, size_t pos, uint16 code, Frame& currentFrame) {
    // RemoveObject (tag 5): CharacterId, Depth
    // RemoveObject2 (tag 28): Depth only
    
    uint16 depth = 0;
    
    if (code == 5) { // REMOVE_OBJECT
        if (pos + 4 > size) return;
        // Skip character ID
        pos += 2;
    }
    
    if (pos + 2 > size) return;
    depth = (data[pos] << 8) | data[pos + 1];
    
    // Create a remove operation
    DisplayItem item;
    item.type = DisplayItem::Type::REMOVE; // Use REMOVE to indicate removal
    item.depth = depth;
    item.characterId = 0;
    
    currentFrame.removeByDepth(depth);
    currentFrame.displayListOps.push_back(item);
}

void SWFParser::parseClipActions(const uint8* data, size_t size, size_t& pos,
                                  std::vector<ClipActionRecord>& actions, bool isPlaceObject3) {
    if (pos + 2 > size) return;

    // Skip reserved field (UI16, always 0)
    pos += 2;

    // Read event flags
    uint32 allEventFlags;
    if (isPlaceObject3) {
        if (pos + 4 > size) return;
        allEventFlags = readU32(data, pos);
    } else {
        if (pos + 2 > size) return;
        allEventFlags = readU16(data, pos);
    }

    (void)allEventFlags;  // Could be used to validate parsed events

    // Parse clip action records until terminator
    while (pos < size) {
        ClipActionRecord record;

        // Event flags
        uint32 eventFlags;
        if (isPlaceObject3) {
            if (pos + 4 > size) break;
            eventFlags = readU32(data, pos);
        } else {
            if (pos + 2 > size) break;
            eventFlags = readU16(data, pos);
        }

        // Check for terminator (all zeros)
        if (eventFlags == 0) {
            break;
        }

        record.eventFlags = eventFlags;

        // Action record size
        if (pos + 4 > size) break;
        uint32 actionSize = readU32(data, pos);

        // Key code (if key press event)
        if (eventFlags & 0x1000) {  // KEY_PRESS
            if (pos + 1 > size) break;
            record.keyCode = data[pos++];
            actionSize--;  // Adjust size
        }

        // Read actions
        if (pos + actionSize > size) break;
        record.actions.assign(data + pos, data + pos + actionSize);
        pos += actionSize;

        actions.push_back(record);
    }
}

void SWFParser::parseFrameLabel(const uint8* data, size_t size, Frame& currentFrame) {
    // Frame label - store in current frame
    std::string label;
    for (size_t i = 0; i < size && data[i] != 0; i++) {
        label += static_cast<char>(data[i]);
    }
    currentFrame.label = label;
}

void SWFParser::parseFileAttributes(const uint8* data, size_t size) {
    if (size < 4) return;
    
    uint32 flags = (data[0] << 24) | (data[1] << 16) | 
                  (data[2] << 8) | data[3];
    
    // Check for AS3
    bool isAS3 = (flags & 0x08) != 0;
    // Check for metadata
    bool hasMetadata = (flags & 0x10) != 0;
    
    // Would store in document
}

void SWFParser::skipTag(const uint8* data, size_t size, size_t pos, uint32 length) {
    // Just skip - already handled by caller incrementing pos
}

// Helper to parse MATRIX from bitstream
Matrix SWFParser::parseMatrix(BitStream& bs) {
    Matrix m;
    
    // Scale
    bool hasScale = bs.readUB(1);
    if (hasScale) {
        int scaleBits = static_cast<int>(bs.readUB(5));
        m.scaleX = bs.readSB(scaleBits) / 65536.0f;
        m.scaleY = bs.readSB(scaleBits) / 65536.0f;
        m.hasScale = true;
    }
    
    // Rotate/Skew
    bool hasRotate = bs.readUB(1);
    if (hasRotate) {
        int rotateBits = static_cast<int>(bs.readUB(5));
        m.rotate0 = bs.readSB(rotateBits) / 65536.0f;
        m.rotate1 = bs.readSB(rotateBits) / 65536.0f;
        m.hasRotate = true;
    }
    
    // Translate
    int translateBits = static_cast<int>(bs.readUB(5));
    m.translateX = bs.readSB(translateBits);
    m.translateY = bs.readSB(translateBits);
    
    return m;
}

std::vector<FillStyle> SWFParser::parseFillStyles(BitStream& bs, int numFill, bool hasAlpha) {
    std::vector<FillStyle> styles;
    
    for (int i = 0; i < numFill; i++) {
        FillStyle style;
        
        uint8 fillType = static_cast<uint8>(bs.readUB(8));
        style.type = static_cast<FillStyle::Type>(fillType);
        
        if (fillType == 0x00) {
            // Solid fill
            style.color.r = static_cast<uint8>(bs.readUB(8));
            style.color.g = static_cast<uint8>(bs.readUB(8));
            style.color.b = static_cast<uint8>(bs.readUB(8));
            if (hasAlpha) {
                style.color.a = static_cast<uint8>(bs.readUB(8));
            } else {
                style.color.a = 255;
            }
        } else if (fillType >= 0x10 && fillType <= 0x13) {
            // Gradient fill
            style.gradientMatrix = parseMatrix(bs);
            bs.alignByte();
            
            // Read focal point for focal radial gradient
            if (fillType == 0x13) {
                int8_t focalByte = static_cast<int8_t>(bs.readUB(8));
                style.focalPoint = focalByte / 255.0f;
            }
            
            // Read gradient stops
            uint8 numStops = static_cast<uint8>(bs.readUB(8));
            style.gradientStops.reserve(numStops);
            for (int j = 0; j < numStops; j++) {
                GradientStop stop;
                stop.ratio = static_cast<uint8>(bs.readUB(8));
                stop.color.r = static_cast<uint8>(bs.readUB(8));
                stop.color.g = static_cast<uint8>(bs.readUB(8));
                stop.color.b = static_cast<uint8>(bs.readUB(8));
                if (hasAlpha) {
                    stop.color.a = static_cast<uint8>(bs.readUB(8));
                } else {
                    stop.color.a = 255;
                }
                style.gradientStops.push_back(stop);
            }
        } else if (fillType >= 0x40) {
            // Bitmap fill
            style.bitmapId = static_cast<uint16>(bs.readUB(16));
            style.bitmapMatrix = parseMatrix(bs);
            bs.alignByte();
        }
        
        styles.push_back(style);
    }
    
    return styles;
}

std::vector<LineStyle> SWFParser::parseLineStyles(BitStream& bs, int numLine, bool hasAlpha) {
    std::vector<LineStyle> styles;
    
    for (int i = 0; i < numLine; i++) {
        LineStyle style;
        
        style.width = static_cast<uint16>(bs.readUB(16));
        style.color.r = static_cast<uint8>(bs.readUB(8));
        style.color.g = static_cast<uint8>(bs.readUB(8));
        style.color.b = static_cast<uint8>(bs.readUB(8));
        if (hasAlpha) {
            style.color.a = static_cast<uint8>(bs.readUB(8));
        } else {
            style.color.a = 255;
        }
        
        styles.push_back(style);
    }
    
    return styles;
}

std::vector<ShapeRecord> SWFParser::parseShapeRecords(BitStream& bs, bool hasAlpha, 
                                                       bool hasNonScalingStrokes) {
    std::vector<ShapeRecord> records;
    
    // Current fill/line style index bit widths (can change with new styles)
    int fillBits = 0;
    int lineBits = 0;
    
    while (bs.hasMoreBits()) {
        // Read first bits to determine record type
        uint8 typeFlag = static_cast<uint8>(bs.readUB(1));
        
        if (typeFlag == 0) {
            // Check for end of shape
            uint8 stateNewStyles = static_cast<uint8>(bs.readUB(1));
            uint8 stateLineStyle = static_cast<uint8>(bs.readUB(1));
            uint8 stateFillStyle1 = static_cast<uint8>(bs.readUB(1));
            uint8 stateFillStyle0 = static_cast<uint8>(bs.readUB(1));
            uint8 stateMoveTo = static_cast<uint8>(bs.readUB(1));
            
            if (stateNewStyles == 0 && stateLineStyle == 0 && 
                stateFillStyle1 == 0 && stateFillStyle0 == 0 && stateMoveTo == 0) {
                // End of shape
                ShapeRecord endRec;
                endRec.type = ShapeRecord::Type::END_SHAPE;
                records.push_back(endRec);
                break;
            }
            
            // Style change record
            ShapeRecord rec;
            rec.type = ShapeRecord::Type::STYLE_CHANGE;
            
            if (stateMoveTo) {
                int nBits = static_cast<int>(bs.readUB(5));
                rec.moveDeltaX = bs.readSB(nBits);
                rec.moveDeltaY = bs.readSB(nBits);
            }
            
            // Read fill style 0 (left side)
            if (stateFillStyle0) {
                rec.fillStyle0 = static_cast<int>(bs.readUB(fillBits));
            }
            
            // Read fill style 1 (right side)
            if (stateFillStyle1) {
                rec.fillStyle1 = static_cast<int>(bs.readUB(fillBits));
            }
            
            // Read line style
            if (stateLineStyle) {
                rec.lineStyle = static_cast<int>(bs.readUB(lineBits));
            }
            
            // New styles (DefineShape2/3 only)
            if (stateNewStyles) {
                rec.newStyles = true;
                // Note: New style arrays would be parsed here and merged
                // This is simplified - in full implementation, we'd parse new arrays
                uint8 numFill = static_cast<uint8>(bs.readUB(8));
                if (numFill == 0xFF) {
                    numFill = static_cast<uint8>(bs.readUB(16));
                }
                parseFillStyles(bs, numFill, hasAlpha);
                
                uint8 numLine = static_cast<uint8>(bs.readUB(8));
                if (numLine == 0xFF) {
                    numLine = static_cast<uint8>(bs.readUB(16));
                }
                parseLineStyles(bs, numLine, hasAlpha);
                
                // Read new bit widths
                fillBits = static_cast<int>(bs.readUB(4));
                lineBits = static_cast<int>(bs.readUB(4));
            }
            
            records.push_back(rec);
        } else {
            // Edge record
            uint8 straightFlag = static_cast<uint8>(bs.readUB(1));
            int nBits = static_cast<int>(bs.readUB(4)) + 2;
            
            if (straightFlag) {
                // Straight edge
                ShapeRecord rec;
                rec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                
                uint8 generalLine = static_cast<uint8>(bs.readUB(1));
                if (generalLine) {
                    rec.anchorDeltaX = bs.readSB(nBits);
                    rec.anchorDeltaY = bs.readSB(nBits);
                } else {
                    uint8 vertOrHoriz = static_cast<uint8>(bs.readUB(1));
                    if (vertOrHoriz) {
                        rec.anchorDeltaY = bs.readSB(nBits);
                    } else {
                        rec.anchorDeltaX = bs.readSB(nBits);
                    }
                }
                
                records.push_back(rec);
            } else {
                // Curved edge
                ShapeRecord rec;
                rec.type = ShapeRecord::Type::CURVED_EDGE;
                
                rec.controlDeltaX = bs.readSB(nBits);
                rec.controlDeltaY = bs.readSB(nBits);
                rec.anchorDeltaX = bs.readSB(nBits);
                rec.anchorDeltaY = bs.readSB(nBits);
                
                records.push_back(rec);
            }
        }
    }
    
    return records;
}

void SWFParser::parseDefineSprite(const uint8* data, size_t size, size_t pos, 
                                   uint16 code, uint32 length) {
    if (pos + 4 > size) return;
    
    auto spriteTag = std::make_unique<SpriteTag>(code, length, pos);
    
    // Read sprite ID and frame count
    spriteTag->spriteId = (data[pos] << 8) | data[pos + 1];
    spriteTag->frameCount = (data[pos + 2] << 8) | data[pos + 3];
    
    // Parse child tags within the sprite
    size_t spritePos = pos + 4;
    size_t spriteEnd = pos + length;
    
    Frame currentFrame;
    currentFrame.frameNumber = 0;
    
    while (spritePos < spriteEnd && spritePos < size) {
        // Read tag header (2 bytes minimum)
        if (spritePos + 2 > size) break;
        
        uint16 tagCodeAndLength = (data[spritePos] << 8) | data[spritePos + 1];
        uint16 tagCode = (tagCodeAndLength >> 6) & 0x3FF;
        uint32 tagLength = tagCodeAndLength & 0x3F;
        
        spritePos += 2;
        
        // Long format
        if (tagLength == 0x3F) {
            if (spritePos + 4 > size) break;
            tagLength = (data[spritePos] << 24) | (data[spritePos + 1] << 16) |
                       (data[spritePos + 2] << 8) | data[spritePos + 3];
            spritePos += 4;
        }
        
        if (spritePos + tagLength > size) break;
        
        // Parse specific tags within sprite (similar to main timeline)
        switch (tagCode) {
            case 0:  // END
                // End of sprite - save last frame if has operations
                if (!currentFrame.displayListOps.empty() || currentFrame.frameNumber == 0) {
                    spriteTag->frames.push_back(currentFrame);
                }
                spritePos = spriteEnd; // Force exit
                continue;
                
            case 1:  // SHOW_FRAME
                // End of current frame, start new one
                spriteTag->frames.push_back(currentFrame);
                currentFrame.frameNumber++;
                currentFrame.displayListOps.clear();
                // Keep the label if set
                break;
                
            case 43: // FRAME_LABEL
                if (tagLength > 0) {
                    std::string label(reinterpret_cast<const char*>(data + spritePos), 
                                     strnlen(reinterpret_cast<const char*>(data + spritePos), tagLength));
                    currentFrame.label = label;
                    spriteTag->frameLabels.push_back(label);
                    spriteTag->frameLabelNumbers.push_back(currentFrame.frameNumber);
                }
                break;
                
            case 4:  // PLACE_OBJECT
            case 26: // PLACE_OBJECT2
            case 70: // PLACE_OBJECT3
                // Parse PlaceObject within sprite context
                parsePlaceObjectInSprite(data, size, spritePos, tagCode, tagLength, currentFrame);
                break;
                
            case 5:  // REMOVE_OBJECT
            case 28: // REMOVE_OBJECT2
                // Parse RemoveObject within sprite context  
                parseRemoveObjectInSprite(data, size, spritePos, tagCode, tagLength, currentFrame);
                break;
                
            case 12: // DO_ACTION
                // Actions within sprite - store reference for later execution
                // For now, we'll store the offset and length
                break;
                
            case 82: // DO_ABC (AS3)
                // ABC bytecode within sprite
                break;
                
            // Shape and other definitions can appear inside sprites
            case 2:  // DEFINE_SHAPE
            case 22: // DEFINE_SHAPE2
            case 32: // DEFINE_SHAPE3
            case 83: // DEFINE_SHAPE4
                // These define characters that can be placed in the sprite
                // We need to parse them and add to document characters
                // but not as part of the timeline
                break;
        }
        
        spritePos += tagLength;
    }
    
    // Ensure frame count matches
    if (spriteTag->frames.empty() && spriteTag->frameCount > 0) {
        // Create empty frames if needed
        for (uint16 i = 0; i < spriteTag->frameCount; i++) {
            Frame f;
            f.frameNumber = i;
            spriteTag->frames.push_back(f);
        }
    }
    
    // Add to document characters
    document_.characters[spriteTag->spriteId] = std::move(spriteTag);
}

void SWFParser::parsePlaceObjectInSprite(const uint8* data, size_t size, size_t pos,
                                           uint16 code, uint32 length, Frame& frame) {
    (void)length;
    
    DisplayItem item;
    
    if (code == 4) { // PLACE_OBJECT (tag 4)
        // Original PlaceObject - simpler format
        if (pos + 4 > size) return;
        item.characterId = (data[pos] << 8) | data[pos + 1];
        item.depth = (data[pos + 2] << 8) | data[pos + 3];
        pos += 4;
        
        // Matrix (required in PlaceObject)
        BitStream bs(data + pos, size - pos);
        item.matrix = parseMatrix(bs);
        item.hasMatrix = true;
        pos += bs.bytePos();
        
        // Color transform (optional, if more data)
        if (pos < size) {
            BitStream bs2(data + pos, size - pos);
            bool hasAddTerms = bs2.readUB(1);
            bool hasMultTerms = bs2.readUB(1);
            int nbits = static_cast<int>(bs2.readUB(4));
            
            if (hasMultTerms) {
                item.cxform.mulR = bs2.readSB(nbits) / 256.0f;
                item.cxform.mulG = bs2.readSB(nbits) / 256.0f;
                item.cxform.mulB = bs2.readSB(nbits) / 256.0f;
            }
            if (hasAddTerms) {
                item.cxform.addR = bs2.readSB(nbits);
                item.cxform.addG = bs2.readSB(nbits);
                item.cxform.addB = bs2.readSB(nbits);
                item.cxform.hasAdd = true;
            }
            item.hasCxform = true;
        }
    } else { // PLACE_OBJECT2 (26) or PLACE_OBJECT3 (70)
        if (pos >= size) return;
        
        // Flags
        uint8 flags = data[pos++];
        bool hasClipActions = (flags >> 7) & 1;
        bool hasClipDepth = (flags >> 6) & 1;
        bool hasName = (flags >> 5) & 1;
        bool hasRatio = (flags >> 4) & 1;
        bool hasColorTransform = (flags >> 3) & 1;
        bool hasMatrix = (flags >> 2) & 1;
        bool hasCharacter = (flags >> 1) & 1;
        bool isMove = flags & 1;
        
        // Depth
        if (pos + 2 > size) return;
        item.depth = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        
        // Character ID
        if (hasCharacter) {
            if (pos + 2 > size) return;
            item.characterId = (data[pos] << 8) | data[pos + 1];
            pos += 2;
        }
        
        // Matrix
        if (hasMatrix) {
            BitStream bs(data + pos, size - pos);
            item.matrix = parseMatrix(bs);
            item.hasMatrix = true;
            pos += bs.bytePos();
        }
        
        // Color transform
        if (hasColorTransform) {
            BitStream bs(data + pos, size - pos);
            bool hasAddTerms = bs.readUB(1);
            bool hasMultTerms = bs.readUB(1);
            int nbits = static_cast<int>(bs.readUB(4));
            
            if (hasMultTerms) {
                item.cxform.mulR = bs.readSB(nbits) / 256.0f;
                item.cxform.mulG = bs.readSB(nbits) / 256.0f;
                item.cxform.mulB = bs.readSB(nbits) / 256.0f;
            }
            if (hasAddTerms) {
                item.cxform.addR = bs.readSB(nbits);
                item.cxform.addG = bs.readSB(nbits);
                item.cxform.addB = bs.readSB(nbits);
                item.cxform.hasAdd = true;
            }
            item.hasCxform = true;
            pos += bs.bytePos();
        }
        
        // Ratio
        if (hasRatio) {
            if (pos + 2 > size) return;
            item.ratio = data[pos + 1];
            item.hasRatio = true;
            pos += 2;
        }
        
        // Instance name
        if (hasName) {
            while (pos < size && data[pos] != 0) {
                item.instanceName += static_cast<char>(data[pos]);
                pos++;
            }
            if (pos < size) pos++;
        }
        
        // Clip depth
        if (hasClipDepth) {
            if (pos + 2 > size) return;
            item.clipDepth = (data[pos] << 8) | data[pos + 1];
            pos += 2;
        }
        
        // Handle move
        if (isMove && !hasCharacter) {
            item.type = DisplayItem::Type::MOVE;
        }
        
        (void)hasClipActions;
    }
    
    // Remove existing and add new
    frame.removeByDepth(item.depth);
    frame.displayListOps.push_back(item);
}

void SWFParser::parseRemoveObjectInSprite(const uint8* data, size_t size, size_t pos,
                                            uint16 code, uint32 length, Frame& frame) {
    (void)length;
    if (pos + 2 > size) return;
    
    uint16 depth;
    if (code == 5) { // REMOVE_OBJECT
        if (pos + 4 > size) return;
        pos += 2; // Skip character ID
    }
    
    depth = (data[pos] << 8) | data[pos + 1];
    
    DisplayItem item;
    item.type = DisplayItem::Type::REMOVE;
    item.depth = depth;
    item.characterId = 0;
    
    frame.removeByDepth(depth);
    frame.displayListOps.push_back(item);
}

void SWFParser::parseDefineSound(const uint8* data, size_t size, size_t pos, 
                                  uint16 code, uint32 length) {
    if (pos + 7 > size) return;
    
    auto soundTag = std::make_unique<SoundTag>(code, length, pos);
    
    // Read sound ID
    soundTag->soundId = (data[pos] << 8) | data[pos + 1];
    
    // Read sound format (1 byte)
    uint8 formatByte = data[pos + 2];
    soundTag->format = static_cast<SoundFormat>((formatByte >> 4) & 0x0F);
    soundTag->rate = static_cast<SoundRate>((formatByte >> 2) & 0x03);
    soundTag->is16Bit = (formatByte >> 1) & 0x01;
    soundTag->isStereo = formatByte & 0x01;
    
    // Read sample count (4 bytes)
    soundTag->sampleCount = (data[pos + 3] << 24) | (data[pos + 4] << 16) |
                           (data[pos + 5] << 8) | data[pos + 6];
    
    // Read sound data
    if (pos + 7 < size) {
        soundTag->soundData.assign(data + pos + 7, data + std::min(pos + length, size));
    }
    
    // Add to document characters
    document_.characters[soundTag->soundId] = std::make_unique<TagBase>(*soundTag);
    
    // Add to tags list
    SWFTag tag(std::move(soundTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseSoundStreamHead(const uint8* data, size_t size, size_t pos, 
                                      uint16 code, uint32 length) {
    if (pos + 4 > size) return;
    
    auto streamTag = std::make_unique<SoundStreamHeadTag>(code, length, pos);
    
    // Read playback format
    uint8 playbackFormat = data[pos];
    streamTag->playbackFormat = static_cast<SoundFormat>((playbackFormat >> 4) & 0x0F);
    streamTag->playbackRate = static_cast<SoundRate>((playbackFormat >> 2) & 0x03);
    streamTag->playbackIs16Bit = (playbackFormat >> 1) & 0x01;
    streamTag->playbackIsStereo = playbackFormat & 0x01;
    
    // Read stream format
    uint8 streamFormat = data[pos + 1];
    streamTag->streamFormat = static_cast<SoundFormat>((streamFormat >> 4) & 0x0F);
    streamTag->streamRate = static_cast<SoundRate>((streamFormat >> 2) & 0x03);
    streamTag->streamIs16Bit = (streamFormat >> 1) & 0x01;
    streamTag->streamIsStereo = streamFormat & 0x01;
    
    // Read samples per block and latency
    streamTag->sampleCount = (data[pos + 2] << 8) | data[pos + 3];
    
    if (streamTag->streamFormat == SoundFormat::MP3) {
        if (pos + 6 > size) return;
        streamTag->latencySeek = (data[pos + 4] << 8) | data[pos + 5];
    }
    
    // Store in document
    SWFTag tag(std::move(streamTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseSoundStreamBlock(const uint8* data, size_t size, size_t pos, uint32 length) {
    auto blockTag = std::make_unique<SoundStreamBlockTag>(19, length, pos);
    
    // Read block data
    if (pos + length <= size) {
        blockTag->blockData.assign(data + pos, data + pos + length);
    }
    
    SWFTag tag(std::move(blockTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseStartSound(const uint8* data, size_t size, size_t pos, 
                                 uint16 code, uint32 length) {
    if (pos + 4 > size) return;
    
    auto startTag = std::make_unique<StartSoundTag>(code, length, pos);
    
    // Read sound ID
    startTag->soundId = (data[pos] << 8) | data[pos + 1];
    
    // Read flags (2 bytes)
    uint16 flags = (data[pos + 2] << 8) | data[pos + 3];
    startTag->stopPlayback = (flags >> 15) & 0x01;
    startTag->noMultiple = (flags >> 14) & 0x01;
    startTag->hasEnvelope = (flags >> 13) & 0x01;
    startTag->hasLoop = (flags >> 12) & 0x01;
    startTag->hasOutPoint = (flags >> 11) & 0x01;
    startTag->hasInPoint = (flags >> 10) & 0x01;
    
    size_t offset = pos + 4;
    
    if (startTag->hasInPoint && offset + 4 <= size) {
        startTag->inPoint = (data[offset] << 24) | (data[offset + 1] << 16) |
                           (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
    }
    
    if (startTag->hasOutPoint && offset + 4 <= size) {
        startTag->outPoint = (data[offset] << 24) | (data[offset + 1] << 16) |
                            (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
    }
    
    if (startTag->hasLoop && offset + 2 <= size) {
        startTag->loopCount = (data[offset] << 8) | data[offset + 1];
    }
    
    SWFTag tag(std::move(startTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineButton(const uint8* data, size_t size, size_t pos, 
                                   uint16 code, uint32 length) {
    if (pos + 2 > size) return;
    
    auto buttonTag = std::make_unique<ButtonTag>(code, length, pos);
    buttonTag->isButton2 = (code == 34);
    buttonTag->trackAsMenu = false;
    
    // Read button ID
    buttonTag->buttonId = (data[pos] << 8) | data[pos + 1];
    
    size_t offset = pos + 2;
    
    size_t actionOffsetPos = 0;
    
    if (buttonTag->isButton2) {
        // DefineButton2 has flags
        if (offset + 1 > size) return;
        uint8 flags = data[offset];
        buttonTag->trackAsMenu = (flags >> 0) & 0x01;
        offset += 1;
        
        // Read action offset
        if (offset + 2 > size) return;
        uint16 actionOffset = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        
        // Remember where actions start (relative to pos)
        if (actionOffset > 0) {
            actionOffsetPos = pos + actionOffset;
        }
    }
    
    // Parse button records
    while (offset < size && offset < pos + length) {
        if (offset + 1 > size) break;
        
        uint8 firstByte = data[offset];
        if (firstByte == 0) {
            offset++; // End of records
            break;
        }
        
        ButtonRecord record;
        record.states = static_cast<ButtonState>(firstByte);
        offset++;
        
        if (offset + 4 > size) break;
        
        record.characterId = (data[offset] << 8) | data[offset + 1];
        record.depth = (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        
        // Read matrix
        BitStream bs(data + offset, size - offset);
        
        // Scale
        bool hasScale = bs.readUB(1);
        if (hasScale) {
            int scaleBits = static_cast<int>(bs.readUB(5));
            record.matrix.scaleX = bs.readSB(scaleBits) / 65536.0f;
            record.matrix.scaleY = bs.readSB(scaleBits) / 65536.0f;
        }
        
        // Rotation
        bool hasRotate = bs.readUB(1);
        if (hasRotate) {
            int rotateBits = static_cast<int>(bs.readUB(5));
            record.matrix.rotate0 = bs.readSB(rotateBits) / 65536.0f;
            record.matrix.rotate1 = bs.readSB(rotateBits) / 65536.0f;
        }
        
        // Translate
        int translateBits = static_cast<int>(bs.readUB(5));
        record.matrix.translateX = bs.readSB(translateBits);
        record.matrix.translateY = bs.readSB(translateBits);
        
        bs.alignByte();
        offset += bs.bytePos();
        
        // For Button2, parse color transform
        if (buttonTag->isButton2) {
            BitStream cxBs(data + offset, size - offset);
            
            bool hasAddTerms = cxBs.readUB(1);
            bool hasMultTerms = cxBs.readUB(1);
            int nBits = static_cast<int>(cxBs.readUB(4));
            
            if (hasMultTerms) {
                record.colorTransform.mulR = cxBs.readSB(nBits) / 256.0f;
                record.colorTransform.mulG = cxBs.readSB(nBits) / 256.0f;
                record.colorTransform.mulB = cxBs.readSB(nBits) / 256.0f;
                record.colorTransform.mulA = cxBs.readSB(nBits) / 256.0f;
            }
            
            if (hasAddTerms) {
                record.colorTransform.addR = cxBs.readSB(nBits);
                record.colorTransform.addG = cxBs.readSB(nBits);
                record.colorTransform.addB = cxBs.readSB(nBits);
                record.colorTransform.addA = cxBs.readSB(nBits);
            }
            
            cxBs.alignByte();
            offset += cxBs.bytePos();
        }
        
        buttonTag->records.push_back(record);
    }
    
    // Parse Button2 actions
    if (buttonTag->isButton2 && actionOffsetPos > 0 && actionOffsetPos < pos + length) {
        offset = actionOffsetPos;
        
        while (offset < pos + length) {
            // Read action header
            if (offset + 1 > size) break;
            uint8 actionType = data[offset++];
            
            if (actionType == 0) {
                // End of actions
                break;
            }
            
            // Read condition flags (2 bytes for Button2)
            if (offset + 2 > size) break;
            uint16 conditionFlags = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            
            // Read action length
            if (offset + 4 > size) break;
            uint32 actionLen = (data[offset] << 24) | (data[offset + 1] << 16) |
                              (data[offset + 2] << 8) | data[offset + 3];
            offset += 4;
            
            if (offset + actionLen > size) break;
            
            // Create action tag
            auto actionTag = std::make_unique<ActionTag>(actionType, actionLen, offset);
            actionTag->conditions = conditionFlags;
            
            // Copy action data
            if (actionLen > 0) {
                actionTag->data.assign(data + offset, data + offset + actionLen);
            }
            
            buttonTag->actions.push_back(std::move(actionTag));
            offset += actionLen;
        }
    }
    
    // Add to document
    document_.characters[buttonTag->buttonId] = std::make_unique<TagBase>(*buttonTag);
    
    SWFTag tag(std::move(buttonTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineButtonSound(const uint8* data, size_t size, size_t pos, uint32 length) {
    if (pos + 2 > size) return;
    
    auto soundTag = std::make_unique<ButtonSoundTag>(17, length, pos);
    soundTag->buttonId = (data[pos] << 8) | data[pos + 1];
    
    size_t offset = pos + 2;
    
    // Parse 4 sound info structures
    auto parseSoundInfo = [&](ButtonSoundTag::SoundInfo& info) {
        if (offset + 2 > size) return;
        info.soundId = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        
        if (info.soundId == 0) return; // No sound
        
        if (offset + 1 > size) return;
        uint8 formatByte = data[offset];
        info.stopPlayback = (formatByte >> 7) & 0x01;
        info.noMultiple = (formatByte >> 6) & 0x01;
        info.hasEnvelope = (formatByte >> 5) & 0x01;
        info.hasLoops = (formatByte >> 4) & 0x01;
        info.hasOutPoint = (formatByte >> 3) & 0x01;
        info.hasInPoint = (formatByte >> 2) & 0x01;
        offset++;
        
        if (info.hasInPoint && offset + 4 <= size) {
            info.inPoint = (data[offset] << 24) | (data[offset + 1] << 16) |
                          (data[offset + 2] << 8) | data[offset + 3];
            offset += 4;
        }
        
        if (info.hasOutPoint && offset + 4 <= size) {
            info.outPoint = (data[offset] << 24) | (data[offset + 1] << 16) |
                           (data[offset + 2] << 8) | data[offset + 3];
            offset += 4;
        }
        
        if (info.hasLoops && offset + 2 <= size) {
            info.loopCount = (data[offset] << 8) | data[offset + 1];
            offset += 2;
        }
    };
    
    parseSoundInfo(soundTag->overUpToIdle);
    parseSoundInfo(soundTag->idleToOverUp);
    parseSoundInfo(soundTag->overUpToOverDown);
    parseSoundInfo(soundTag->overDownToOverUp);
    
    SWFTag tag(std::move(soundTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineButtonCxform(const uint8* data, size_t size, size_t pos, uint32 length) {
    // DefineButtonCxform is deprecated, just store the button ID reference
    if (pos + 2 > size) return;
    
    uint16 buttonId = (data[pos] << 8) | data[pos + 1];
    
    // In a full implementation, we would store the color transform
    // For now, we just skip this tag (it's rarely used)
    (void)buttonId;
    (void)length;
}

void SWFParser::parseDefineVideoStream(const uint8* data, size_t size, size_t pos, uint32 length) {
    if (pos + 10 > size) return;
    
    auto videoTag = std::make_unique<VideoStreamTag>(60, length, pos);
    
    size_t offset = pos;
    
    // Character ID (2 bytes, little-endian)
    videoTag->characterId = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Number of frames (2 bytes)
    videoTag->numFrames = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Video width (2 bytes)
    videoTag->width = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Video height (2 bytes)
    videoTag->height = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Video flags (1 byte)
    // Bits:
    //   0-3: Reserved (0)
    //   4-5: Video flags deblocking (0 = use VIDEOPACKET value)
    //   6:   Video flags smoothing (0 = off, 1 = on)
    //   7:   Reserved (0)
    if (offset < size) {
        uint8 flags = data[offset];
        videoTag->smoothDeblocking = (flags >> 1) & 0x01;
        offset++;
    }
    
    // Codec ID (1 byte)
    if (offset < size) {
        videoTag->codec = static_cast<VideoCodec>(data[offset]);
        offset++;
    }
    
    // Add to document
    document_.characters[videoTag->characterId] = std::make_unique<TagBase>(*videoTag);
    
    SWFTag tag(std::move(videoTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseVideoFrame(const uint8* data, size_t size, size_t pos, uint32 length) {
    if (pos + 4 > size) return;
    
    auto frameTag = std::make_unique<VideoFrameTag>(61, length, pos);
    
    size_t offset = pos;
    
    // Stream ID (2 bytes, little-endian)
    frameTag->streamId = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Frame number (2 bytes)
    frameTag->frameNum = data[offset] | (data[offset + 1] << 8);
    offset += 2;
    
    // Video data
    size_t dataSize = length - 4;
    if (offset + dataSize <= size) {
        frameTag->videoData.resize(dataSize);
        memcpy(frameTag->videoData.data(), data + offset, dataSize);
    }
    
    SWFTag tag(std::move(frameTag));
    document_.tags.push_back(std::move(tag));
}

void SWFParser::parseDefineMorphShape(const uint8* data, size_t size, size_t pos, 
                                       uint32 length) {
    (void)length;
    size_t offset = pos;
    
    if (offset + 2 > size) return;
    
    auto morphTag = std::make_unique<MorphShapeTag>(46, length, offset);
    
    // Character ID
    morphTag->characterId = (data[offset] << 8) | data[offset + 1];
    offset += 2;
    
    // Start bounds
    BitStream bs1(data + offset, size - offset);
    int nBits = static_cast<int>(bs1.readUB(5));
    morphTag->startBounds.xMin = bs1.readSB(nBits);
    morphTag->startBounds.xMax = bs1.readSB(nBits);
    morphTag->startBounds.yMin = bs1.readSB(nBits);
    morphTag->startBounds.yMax = bs1.readSB(nBits);
    bs1.alignByte();
    offset += bs1.bytePos();
    
    // End bounds
    BitStream bs2(data + offset, size - offset);
    nBits = static_cast<int>(bs2.readUB(5));
    morphTag->endBounds.xMin = bs2.readSB(nBits);
    morphTag->endBounds.xMax = bs2.readSB(nBits);
    morphTag->endBounds.yMin = bs2.readSB(nBits);
    morphTag->endBounds.yMax = bs2.readSB(nBits);
    bs2.alignByte();
    offset += bs2.bytePos();
    
    // Skip offset to EndEdges (4 bytes) - points to end shape data
    if (offset + 4 > size) return;
    uint32_t endEdgesOffset = (data[offset] << 24) | (data[offset + 1] << 16) |
                               (data[offset + 2] << 8) | data[offset + 3];
    (void)endEdgesOffset;
    offset += 4;
    
    // Calculate fill bits and line bits for start shape
    BitStream startBs(data + offset, size - offset);
    int fillBits = static_cast<int>(startBs.readUB(4));
    int lineBits = static_cast<int>(startBs.readUB(4));
    startBs.alignByte();
    
    // Parse start shape records
    morphTag->records.clear();
    while (startBs.hasMoreBits()) {
        ShapeRecord rec;
        uint8 typeFlag = static_cast<uint8>(startBs.readUB(1));
        
        if (typeFlag == 0) {
            // Style change or end
            uint8 flags = static_cast<uint8>(startBs.readUB(5));
            if (flags == 0) break; // End shape
            
            rec.type = ShapeRecord::Type::STYLE_CHANGE;
            if (flags & 0x10) {
                int moveBits = static_cast<int>(startBs.readUB(5));
                rec.moveDeltaX = startBs.readSB(moveBits);
                rec.moveDeltaY = startBs.readSB(moveBits);
            }
            rec.fillStyle0 = (flags & 0x08) ? startBs.readUB(fillBits) : 0;
            rec.fillStyle1 = (flags & 0x04) ? startBs.readUB(fillBits) : 0;
            rec.lineStyle = (flags & 0x02) ? startBs.readUB(lineBits) : 0;
            // New styles flag would require more parsing
        } else {
            uint16 straightFlag = static_cast<uint16>(startBs.readUB(1));
            int nBits = static_cast<int>(startBs.readUB(4)) + 2;
            
            if (straightFlag == 1) {
                rec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                bool generalLine = startBs.readUB(1);
                if (generalLine) {
                    rec.anchorDeltaX = startBs.readSB(nBits);
                    rec.anchorDeltaY = startBs.readSB(nBits);
                } else {
                    bool vertLine = startBs.readUB(1);
                    if (vertLine) {
                        rec.anchorDeltaX = 0;
                        rec.anchorDeltaY = startBs.readSB(nBits);
                    } else {
                        rec.anchorDeltaX = startBs.readSB(nBits);
                        rec.anchorDeltaY = 0;
                    }
                }
            } else {
                rec.type = ShapeRecord::Type::CURVED_EDGE;
                rec.controlDeltaX = startBs.readSB(nBits);
                rec.controlDeltaY = startBs.readSB(nBits);
                rec.anchorDeltaX = startBs.readSB(nBits);
                rec.anchorDeltaY = startBs.readSB(nBits);
            }
        }
        
        MorphShapeRecord morphRec;
        morphRec.startRecord = rec;
        morphTag->records.push_back(morphRec);
    }
    
    // Store in characters
    document_.characters[morphTag->characterId] = std::move(morphTag);
}

} // namespace libswf
