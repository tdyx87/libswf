// 软件光栅器：把解码后的 Movie 某一帧渲染成 RGBA 缓冲并写出 PNG。
// 不依赖任何 GPU/GL，纯 CPU，跨平台（含 CI / 移动端）可直接运行。
#pragma once
#include <cstdint>
#include <string>
#include "swf/Decoder.hpp"

namespace swf {
// 渲染 movie 的第 frameIndex 帧并写出 PNG。返回是否成功。
bool renderMovieToPNG(const Movie& movie, int frameIndex, const char* path);
}  // namespace swf
