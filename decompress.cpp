#include "decompress.h"
#include <zlib.h>
#include <vector>
#if 0
std::vector<uint8_t> zlibdecompressData(
    const std::vector<uint8_t>& compressedData)
{
    if (compressedData.empty()) {
        return std::vector<uint8_t>();
    }

    // 初始化 zlib 流
    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;

    // 初始化解压（使用 inflate）
    int ret = inflateInit(&stream);
    if (ret != Z_OK) {
        throw std::runtime_error("inflateInit 失败");
    }

    std::vector<uint8_t> decompressedData;
    const size_t CHUNK_SIZE = 16384;  // 16KB 块大小
    std::vector<uint8_t> outBuffer(CHUNK_SIZE);

    // 设置输入数据
    stream.avail_in = compressedData.size();
    stream.next_in = const_cast<Bytef*>(compressedData.data());

    // 解压循环
    // do {
    stream.avail_out = CHUNK_SIZE;
    stream.next_out = outBuffer.data();

    ret = inflate(&stream, Z_NO_FLUSH);

    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        inflateEnd(&stream);
        throw std::runtime_error("解压过程中发生错误");
    }

    // 将解压出的数据添加到结果中
    size_t have = CHUNK_SIZE - stream.avail_out;
    if (have > 0) {
        decompressedData.insert(decompressedData.end(), outBuffer.begin(),
                                outBuffer.begin() + have);
    }
    //} while (ret != Z_STREAM_END);

    // 清理
    inflateEnd(&stream);

    return decompressedData;
}
#endif
int ZlibDecompress::Init()
{
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;

    // 初始化解压（使用 inflate）
    int ret = inflateInit(&stream);
    return ret;
}

int ZlibDecompress::Decompress(const std::vector<uint8_t>& in,
                               std::vector<uint8_t>& out)
{
    const size_t CHUNK_SIZE = 16384;  // 16KB 块大小
    std::vector<uint8_t> outBuffer(CHUNK_SIZE);

    // 设置输入数据
    stream.avail_in = in.size();
    stream.next_in = const_cast<Bytef*>(in.data());

    stream.avail_out = CHUNK_SIZE;
    stream.next_out = outBuffer.data();

    int ret = inflate(&stream, Z_NO_FLUSH);

    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        inflateEnd(&stream);
        return -1;
    }

    // 将解压出的数据添加到结果中
    size_t have = CHUNK_SIZE - stream.avail_out;
    if (have > 0) {
        out.insert(out.end(), outBuffer.begin(), outBuffer.begin() + have);
    }
    return ret;
}

int ZlibDecompress::End()
{
    inflateEnd(&stream);
    return Z_OK;
}
