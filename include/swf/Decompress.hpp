// 对照逆向结果：sub_4797C0 一次喂数据 -> sub_A1F02B（静态链接的 zlib inflate）
//   'C' (CWS) => zlib 流；'Z' (ZWS) => LZMA alone 流（模式由 movie+1432 选择）
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swf {

bool inflateZlib(const std::uint8_t* src, std::size_t srcLen, std::vector<std::uint8_t>& out);

// ZWS：从 offset 8 起为 LZMA alone 流（5 字节 props + 8 字节原始长度 + 数据）
bool decodeLzmaAlone(const std::uint8_t* src, std::size_t srcLen, std::vector<std::uint8_t>& out);

}  // namespace swf
