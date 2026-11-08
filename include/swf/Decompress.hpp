// 对照逆向结果：sub_4797C0 一次喂数据 -> sub_A1F02B（静态链接的 zlib inflate）
//   'C' (CWS) => zlib 流；'Z' (ZWS) => LZMA 压缩流
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swf {

// 单个压缩流的输出上限（防御 zlib/LZMA 解压炸弹）。SWF 头里的 FileLength
// 本应作为精确上限，但样本中存在该字段不准确的文件，故阶段 1 先用宽松硬上限，
// 阶段 2 再按 FileLength 做精确校验。
inline constexpr std::size_t kMaxDecompressedBytes = 256ull * 1024ull * 1024ull;

bool inflateZlib(const std::uint8_t* src, std::size_t srcLen,
                 std::vector<std::uint8_t>& out,
                 std::size_t maxOut = kMaxDecompressedBytes);

// ZWS body（从文件 offset 8 起）= UI32 CompressedSize + 5 字节 LZMA1 props
// + LZMA_RAW 数据流。它不是 .lzma alone 格式（没有 8 字节原始长度字段），
// 必须用 lzma_raw_decoder 解码，旧实现按 alone 解码会整体错位 4 字节。
bool decodeZws(const std::uint8_t* src, std::size_t srcLen,
               std::vector<std::uint8_t>& out,
               std::size_t maxOut = kMaxDecompressedBytes);

}  // namespace swf
