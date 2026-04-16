#pragma once
#include <stdint.h>
#include <vector>
#include <zlib.h>
class ZlibDecompress {
public:
    int Init();
    int Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);
    int End();

private:
    z_stream stream;
};

std::vector<uint8_t> zlibdecompressData(
    const std::vector<uint8_t>& compressedData);