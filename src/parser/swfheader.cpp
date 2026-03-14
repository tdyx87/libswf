#include "parser/swfheader.h"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <zlib.h>

// LZMA support - try to include lzma.h if available
#if __has_include(<lzma.h>)
    #define HAS_LZMA 1
    #include <lzma.h>
#else
    #define HAS_LZMA 0
#endif

namespace libswf {

SWFHeader SWFHeaderParser::parse(const std::vector<uint8>& data) {
    if (data.size() < 12) {
        throw std::runtime_error("SWF file too small");
    }
    
    SWFHeader header;
    
    // Read signature (3 bytes)
    header.signature = std::string(reinterpret_cast<const char*>(&data[0]), 3);
    
    if (header.signature != "FWS" && 
        header.signature != "CWS" && 
        header.signature != "ZWS") {
        throw std::runtime_error("Invalid SWF signature: " + header.signature);
    }
    
    header.compressed = (header.signature == "CWS" || header.signature == "ZWS");
    
    // Read version
    header.version = data[3];
    
    // Read file length (little-endian, same as x86)
    memcpy(&header.fileLength, &data[4], 4);
    
    // For ZWS (LZMA), the header is larger
    size_t headerOffset = 8;
    if (header.signature == "ZWS") {
        // ZWS has additional LZMA properties before the RECT
        headerOffset = 17;  // 8 + 4 (compressed size) + 5 (LZMA properties)
    }
    
    // Parse frame size (RECT)
    BitStream bs(data.data() + headerOffset, data.size() - headerOffset);
    int nBits = static_cast<int>(bs.readUB(5));
    
    header.frameSize.xMin = bs.readSB(nBits);
    header.frameSize.xMax = bs.readSB(nBits);
    header.frameSize.yMin = bs.readSB(nBits);
    header.frameSize.yMax = bs.readSB(nBits);
    
    // Align to byte boundary
    bs.alignByte();
    
    // Read frame rate (fixed8) - little endian
    size_t pos = bs.bytePos();
    uint16 frameRateValue = data[headerOffset + pos] | (data[headerOffset + pos + 1] << 8);
    header.frameRate = frameRateValue / 256.0f;
    pos += 2;
    
    // Read frame count - little endian
    header.frameCount = data[headerOffset + pos] | (data[headerOffset + pos + 1] << 8);
    pos += 2;
    
    // Calculate total header size
    header.headerSize = headerOffset + pos;
    
    return header;
}

SWFHeader SWFHeaderParser::parseFromFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    
    // Read entire file
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();
    
    // If compressed, decompress
    SWFHeader header = parse(data);
    
    if (header.compressed) {
        std::vector<uint8> decompressed;
        
        if (header.signature == "CWS") {
            // Zlib compression (CWS = Compressed SWF)
            uint32 compressedSize = fileSize - 8;  // Skip first 8 bytes (signature + version + length)
            uint32 uncompressedSize = header.fileLength - 8;
            decompressed = decompressZlib(data.data() + 8, compressedSize, uncompressedSize);
        } else if (header.signature == "ZWS") {
            // LZMA compression (ZWS = ZLIB SWF, actually LZMA)
            uint32 compressedSize;
            memcpy(&compressedSize, data.data() + 8, 4);  // Compressed size at offset 8
            uint32 uncompressedSize = header.fileLength - 8;
            decompressed = decompressLZMA(data.data() + 12, compressedSize, uncompressedSize);
        }
        
        // Reconstruct the file with "FWS" signature
        if (!decompressed.empty()) {
            data = decompressed;
            data[0] = 'F';  // Change signature to uncompressed
            header.signature = "FWS";
        }
    }
    
    return header;
}

std::vector<uint8> SWFHeaderParser::decompressZlib(const uint8* compressed,
                                                       uint32 compressedSize,
                                                       uint32 uncompressedSize) {
    std::vector<uint8> result(uncompressedSize);
    
    z_stream stream = {};
    stream.next_in = const_cast<uint8*>(compressed);
    stream.avail_in = compressedSize;
    stream.next_out = result.data();
    stream.avail_out = uncompressedSize;
    
    int ret = inflateInit2(&stream, -MAX_WBITS);  // Raw deflate
    if (ret != Z_OK) {
        throw std::runtime_error("Failed to initialize zlib");
    }
    
    ret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("Failed to decompress zlib data");
    }
    
    return result;
}

std::vector<uint8> SWFHeaderParser::decompressLZMA(const uint8* compressed,
                                                       uint32 compressedSize,
                                                       uint32 uncompressedSize) {
#if HAS_LZMA
    // Using xz-utils liblzma library
    std::vector<uint8> result(uncompressedSize);
    
    // SWF uses a specific LZMA format with 5-byte properties at the beginning
    // The properties are: 5 bytes of LZMA properties header
    const uint8* lzmaProps = compressed;  // 5 bytes: dictSize (4 bytes) + pb, lp, lc (1 byte)
    const uint8* lzmaData = compressed + 5;
    uint32 lzmaDataSize = compressedSize - 5;
    
    // Allocate lzma stream
    lzma_stream stream = LZMA_STREAM_INIT;
    
    // Initialize raw LZMA decoder
    // Parse properties
    uint32_t dictSize = lzmaProps[0] | (lzmaProps[1] << 8) | (lzmaProps[2] << 16) | (lzmaProps[3] << 24);
    uint8_t propsByte = lzmaProps[4];
    
    // Calculate pb, lp, lc from props byte
    uint8_t pb = propsByte / (9 * 5);
    propsByte %= (9 * 5);
    uint8_t lp = propsByte / 9;
    uint8_t lc = propsByte % 9;
    
    lzma_options_lzma opt;
    opt.dict_size = dictSize;
    opt.preset_dict = nullptr;
    opt.preset_dict_size = 0;
    opt.lc = lc;
    opt.lp = lp;
    opt.pb = pb;
    opt.mode = LZMA_MODE_NORMAL;
    opt.nice_len = 128;
    opt.mf = LZMA_MF_BT4;
    opt.depth = 0;
    
    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA1;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    
    lzma_ret ret = lzma_raw_decoder(&stream, filters);
    if (ret != LZMA_OK) {
        throw std::runtime_error("Failed to initialize LZMA decoder");
    }
    
    stream.next_in = lzmaData;
    stream.avail_in = lzmaDataSize;
    stream.next_out = result.data();
    stream.avail_out = uncompressedSize;
    
    ret = lzma_code(&stream, LZMA_FINISH);
    lzma_end(&stream);
    
    if (ret != LZMA_STREAM_END) {
        throw std::runtime_error("Failed to decompress LZMA data");
    }
    
    return result;
#else
    // Fallback: Try to use system lzma command if available
    // This is a temporary solution when liblzma is not linked
    
    // Save compressed data to temp file
    char tempIn[] = "/tmp/swf_lzma_in_XXXXXX";
    char tempOut[] = "/tmp/swf_lzma_out_XXXXXX";
    
    int fdIn = mkstemp(tempIn);
    int fdOut = mkstemp(tempOut);
    
    if (fdIn < 0 || fdOut < 0) {
        if (fdIn >= 0) close(fdIn);
        if (fdOut >= 0) close(fdOut);
        throw std::runtime_error("LZMA compression not available. "
                                "Please install liblzma-dev and rebuild.");
    }
    
    // SWF LZMA format has 5 bytes of properties before compressed data
    // Write in xz format (lzma alone format is tricky)
    write(fdIn, compressed, compressedSize);
    close(fdIn);
    close(fdOut);
    
    // Try using lzma command line tool
    std::string cmd = "lzma -d -k " + std::string(tempIn) + " 2>/dev/null || true";
    int systemRet = system(cmd.c_str());
    (void)systemRet;  // Ignore return value
    
    // Try to read decompressed file
    std::string decompressedName = std::string(tempIn) + ".out";
    std::ifstream decompFile(decompressedName, std::ios::binary);
    
    std::vector<uint8> result;
    if (decompFile) {
        decompFile.seekg(0, std::ios::end);
        size_t size = decompFile.tellg();
        decompFile.seekg(0, std::ios::beg);
        result.resize(size);
        decompFile.read(reinterpret_cast<char*>(result.data()), size);
        decompFile.close();
    }
    
    // Cleanup
    unlink(tempIn);
    unlink(tempOut);
    unlink(decompressedName.c_str());
    
    if (result.empty()) {
        throw std::runtime_error("LZMA decompression failed. "
                                "Please install liblzma-dev and rebuild with LZMA support.");
    }
    
    return result;
#endif
}

} // namespace libswf
