// 软件光栅器：把解码后的 Movie 某一帧渲染成 RGBA 缓冲并写出 PNG。
// 不依赖任何 GPU/GL，纯 CPU，跨平台（含 CI / 移动端）可直接运行。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "swf/Decoder.hpp"

namespace swf {
// 渲染 movie 的第 frameIndex 帧并写出 PNG。返回是否成功。
bool renderMovieToPNG(const Movie& movie, int frameIndex, const char* path);

// 与 renderMovieToPNG 相同的光栅化，但把 RGBA 写入 out（W*H*4），供像素级断言/测试。
// 返回是否成功（frameIndex 越界返回 false）。
bool renderMovieToBuffer(const Movie& movie, int frameIndex,
                         std::vector<std::uint8_t>& out, int& W, int& H);

// 用**指定的显示列表**（Player 已应用运行时覆盖/按钮状态的可变副本）渲染一帧。
// items 的 character 前向引用已解析；frameIndex 用于精灵内部帧对齐。
// 背景/尺寸与 renderMovieToBuffer 一致；AVM2 runtimeStage 照常叠加。
bool renderFrameWithItems(const Movie& movie, const std::vector<DisplayItem>& items,
                          int frameIndex,
                          std::vector<std::uint8_t>& out, int& W, int& H);

// 命中测试：在给定显示列表上检测舞台像素点 (x, y)（左上原点）命中的最上层项。
// 返回 items 下标；未命中返回 -1。递归进入 Sprite 内部帧与 Button 的 Hit 态。
int hitTestItems(const Movie& movie, const std::vector<DisplayItem>& items,
                 double x, double y, int frameIndex);

// 命中测试（递归解析到最上层可交互项）：chain 为自顶层到命中项的显示项链条
// （顶层 -> … -> 命中的 Button/EditText/精灵）。命中返回 true。
// 与 hitTestItems 不同，本函数会深入 Sprite 内部，从而定位到嵌套的 Button
// （swf.swf 的商店/结算界面按钮都嵌在精灵中，顶层扫描无法命中）。
bool hitTestItemsDeep(const Movie& movie, const std::vector<DisplayItem>& items,
                      double x, double y, int frameIndex,
                      std::vector<const DisplayItem*>& chain);

}  // namespace swf
