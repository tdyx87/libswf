// 对照逆向结果：SWF 文件头与标签头
//   sub_490330：sig[0] ∈ {'F','C','Z'} && sig[1]=='W' && sig[2]=='S'
//               version = byte[3]（0 非法，8 有特殊标记）
//               FileLength = UI32（小端，< 8 非法）
//               'C' → zlib 解压器，'Z' → LZMA 解压器
//               解析完成后 body 起点 = 8（*(v5+84) = *(v5+8) = 8）
//   sub_47A550：UI16 → tagCode = v >> 6, length = v & 0x3F；length == 63 时读 UI32
#pragma once

#include <cstdint>
#include <string>
#include "swf/BitReader.hpp"  // 带来 Rect

namespace swf {

enum class Compression { Uncompressed, Zlib, Lzma };

struct SwfHeader {
    char signature[4] = {0, 0, 0, 0};  // "FWS" / "CWS" / "ZWS"
    std::uint8_t version = 0;
    std::uint32_t fileLength = 0;
    Compression compression = Compression::Uncompressed;

    bool valid() const {
        bool firstOk = signature[0] == 'F' || signature[0] == 'C' || signature[0] == 'Z';
        return firstOk && signature[1] == 'W' && signature[2] == 'S';
    }
};

struct Rect2 {
    std::int32_t xMin = 0, xMax = 0, yMin = 0, yMax = 0;
};

// 头之后的 body 结构：RECT(frameSize) + UI16(frameRate) + UI16(frameCount)
struct SwfBodyHeader {
    Rect frameSize;
    std::uint16_t frameRateRaw = 0;   // 8.8 定点
    std::uint16_t frameCount = 0;
    double frameRate() const { return frameRateRaw / 256.0; }
};

struct TagHeader {
    std::uint16_t code = 0;
    std::uint32_t length = 0;
    std::size_t dataOffset = 0;   // 标签数据在缓冲中的起始（对应 this+12）
    std::size_t dataEnd = 0;      // 对应 this+3 = length + pos
};

}  // namespace swf
