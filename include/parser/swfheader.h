#ifndef LIBSWF_PARSER_SWFHEADER_H
#define LIBSWF_PARSER_SWFHEADER_H

#include "common/types.h"
#include "common/bitstream.h"
#include <memory>

namespace libswf {

// SWF file header
struct SWFHeader {
    std::string signature;  // "FWS", "CWS", or "ZWS"
    uint8 version;           // SWF version (1-30+)
    uint32 fileLength;       // Uncompressed file length
    Rect frameSize;          // Frame bounds in TWIPS
    float32 frameRate;       // Frame rate (in frames per second)
    uint16 frameCount;       // Total number of frames in file
    bool compressed;         // Whether the file is compressed
    uint32 headerSize;       // Size of header in bytes (including RECT, frame rate, frame count)
    
    SWFHeader() : version(0), fileLength(0), frameRate(0.0f), 
                  frameCount(0), compressed(false), headerSize(0) {}
};

// Parse SWF header from file data
class SWFHeaderParser {
public:
    static SWFHeader parse(const std::vector<uint8>& data);
    static SWFHeader parseFromFile(const std::string& filename);
    
    // For CWS (zlib compressed), decompress after header
    static std::vector<uint8> decompressZlib(const uint8* compressed, 
                                               uint32 compressedSize, 
                                               uint32 uncompressedSize);
    // For ZWS (LZMA compressed)
    static std::vector<uint8> decompressLZMA(const uint8* compressed,
                                              uint32 compressedSize,
                                              uint32 uncompressedSize);
};

} // namespace libswf

#endif // LIBSWF_PARSER_SWFHEADER_H
