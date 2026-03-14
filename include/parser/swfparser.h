#ifndef LIBSWF_PARSER_SWFPARSER_H
#define LIBSWF_PARSER_SWFPARSER_H

#include "parser/swfheader.h"
#include "parser/swftag.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace libswf {

// Frame and DisplayItem are now defined in swftag.h

// Complete SWF document
class SWFDocument {
public:
    SWFHeader header;
    std::vector<SWFTag> tags;
    RGBA backgroundColor;
    
    // Symbol table for DefineSprite/DefineButton
    std::unordered_map<uint16, std::unique_ptr<TagBase>> characters;
    
    // Exported assets
    std::vector<ExportedAsset> exports;
    std::unordered_map<std::string, uint16> exportNameToId;
    
    // ABC files (ActionScript 3)
    std::vector<std::unique_ptr<ABCFileTag>> abcFiles;
    
    // Symbol class (linkage)
    std::unordered_map<uint16, std::string> symbolClass;
    
    // Frame data - stores display list state for each frame
    std::vector<Frame> frames;
    
    // Background color set?
    bool hasBackgroundColor = false;
    
    SWFDocument() : backgroundColor(255, 255, 255, 255) {}
    
    // Disable copy (because of unique_ptr members)
    SWFDocument(const SWFDocument&) = delete;
    SWFDocument& operator=(const SWFDocument&) = delete;
    
    // Enable move
    SWFDocument(SWFDocument&& other) noexcept
        : header(other.header)
        , tags(std::move(other.tags))
        , backgroundColor(other.backgroundColor)
        , characters(std::move(other.characters))
        , exports(std::move(other.exports))
        , exportNameToId(std::move(other.exportNameToId))
        , abcFiles(std::move(other.abcFiles))
        , symbolClass(std::move(other.symbolClass))
        , hasBackgroundColor(other.hasBackgroundColor) {}
    
    SWFDocument& operator=(SWFDocument&& other) noexcept {
        if (this != &other) {
            header = other.header;
            tags = std::move(other.tags);
            backgroundColor = other.backgroundColor;
            characters = std::move(other.characters);
            exports = std::move(other.exports);
            exportNameToId = std::move(other.exportNameToId);
            abcFiles = std::move(other.abcFiles);
            symbolClass = std::move(other.symbolClass);
            hasBackgroundColor = other.hasBackgroundColor;
        }
        return *this;
    }
    
    // Get character by ID
    TagBase* getCharacter(uint16 id) {
        auto it = characters.find(id);
        if (it != characters.end()) {
            return it->second.get();
        }
        return nullptr;
    }
    
    // Get export by name
    uint16 getExportId(const std::string& name) const {
        auto it = exportNameToId.find(name);
        if (it != exportNameToId.end()) {
            return it->second;
        }
        return 0;
    }
};

// SWF Parser
class SWFParser {
public:
    SWFParser();
    ~SWFParser();
    
    // Parse from file
    bool parseFile(const std::string& filename);
    
    // Parse from memory
    bool parse(const uint8* data, size_t size);
    bool parse(const std::vector<uint8>& data);
    
    // Get parsed document
    SWFDocument& getDocument() { return document_; }
    const SWFDocument& getDocument() const { return document_; }
    
    // Move document out (for transfering ownership)
    SWFDocument extractDocument() { return std::move(document_); }
    
    // Get error message
    const std::string& getError() const { return error_; }
    
    // Check if parsing was successful
    bool isValid() const { return valid_; }
    
private:
    SWFDocument document_;
    std::string error_;
    bool valid_ = false;
    
    // Internal parsing
    bool parseInternal(const std::vector<uint8>& data);
    
    void parseTag(const uint8* data, size_t dataSize, size_t& pos, Frame& currentFrame);
    
    // Current frame tracking
    uint16 currentFrameNum_ = 0;
    
    // Tag parsers
    void parseDefineShape(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineShape2(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineShape3(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineBits(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineBitsLossless(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineFont(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineFont2(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineFont3(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineText(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineEditText(const uint8* data, size_t size, size_t& pos, uint16 code, uint32 length);
    void parseDoAction(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseDoABC(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseSetBackgroundColor(const uint8* data, size_t size);
    void parseExportAssets(const uint8* data, size_t size);
    void parseSymbolClass(const uint8* data, size_t size);
    void parsePlaceObject(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length, Frame& currentFrame);
    void parsePlaceObject1(const uint8* data, size_t size, size_t pos, Frame& currentFrame);
    void parsePlaceObject2(const uint8* data, size_t size, size_t pos, Frame& currentFrame);
    void parsePlaceObject3(const uint8* data, size_t size, size_t pos, Frame& currentFrame);
    void parseRemoveObject(const uint8* data, size_t size, size_t pos, uint16 code, Frame& currentFrame);
    void parseFrameLabel(const uint8* data, size_t size, Frame& currentFrame);
    void parseFileAttributes(const uint8* data, size_t size);
    void parseDefineSprite(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineSound(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseSoundStreamHead(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseSoundStreamBlock(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseStartSound(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineButton(const uint8* data, size_t size, size_t pos, uint16 code, uint32 length);
    void parseDefineButtonSound(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseDefineButtonCxform(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseDefineMorphShape(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseClipActions(const uint8* data, size_t size, size_t& pos, std::vector<ClipActionRecord>& actions, bool isPlaceObject3);
    
    // Sprite parsing helpers
    void parsePlaceObjectInSprite(const uint8* data, size_t size, size_t pos, 
                                   uint16 code, uint32 length, Frame& frame);
    void parseRemoveObjectInSprite(const uint8* data, size_t size, size_t pos,
                                    uint16 code, uint32 length, Frame& frame);
    
    // Video tags
    void parseDefineVideoStream(const uint8* data, size_t size, size_t pos, uint32 length);
    void parseVideoFrame(const uint8* data, size_t size, size_t pos, uint32 length);
    
    // Helper methods
    std::vector<FillStyle> parseFillStyles(BitStream& bs, int numFill, bool hasAlpha);
    std::vector<LineStyle> parseLineStyles(BitStream& bs, int numLine, bool hasAlpha);
    std::vector<ShapeRecord> parseShapeRecords(BitStream& bs, bool hasAlpha, bool hasNonScalingStrokes);
    
    void skipTag(const uint8* data, size_t size, size_t pos, uint32 length);
    
    // Helper to parse matrix from bitstream
    static Matrix parseMatrix(BitStream& bs);
};

} // namespace libswf

#endif // LIBSWF_PARSER_SWFPARSER_H
