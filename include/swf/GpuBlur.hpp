#pragma once
// 方案 B：GPU 可分离高斯模糊（D3D11 计算着色器，无 d3dx9 依赖）。
//
// 分工（逐位复现 CPU 管线的前提）：
//   - 降采样（box d=2/4）与升采样（双线性）留在 CPU（O(N)、算术不变）；
//   - GPU 只承担两趟核卷积（水平/垂直，r=ceil(3σ') taps，边缘 clamp 采样），
//     即 CPU 版 blurBufferCore / blurPlane 的 H/V 两趟。
//
// 逐位一致性依据（2026-11 独立冒烟 d3d11_diag2..7 逐条验证）：
//   - 计算着色器 cs_5_0 对 StructuredBuffer<float4>（R32G32B32A32_FLOAT，
//     16B/像素）读写：0..255 整数 float 精确表示，整条链无 unorm 舍入；
//   - 累加顺序 -R..+R 与 CPU 循环逐 tap 相同（acc += src*k，float4 分通道）；
//   - 输出 truncByte = clamp+floor ≡ CPU 的 clampU8((int)(acc+0.5f))；
//   - 冒烟 4 组随机输入（含 r=2/6/9/24）119032 通道 diff=0 maxDelta=0；
//   - 512x512 R=24 完整往返 GPU 15.7ms vs CPU 67.0ms（4.3x）。
// 注意：本机 D3D11 光栅化器（Draw）静默失效（虚拟化 GPU），故采用 Dispatch；
// 真实 GPU 上 Dispatch 同样可用且更快。
//
// 任何初始化失败 / 尺寸超限 / 运行错误 → 返回 false，调用方回退 CPU 核心，
// 输出不受影响。开关 SWF_GPU_BLUR（默认 ON；"0"/"off"/"no" 关闭）、
// 最小窗口 SWF_GPU_BLUR_MIN_PX（默认 1024 像素，小窗 GPU 往返不划算）。
#include <cstdint>
#include <vector>

namespace swf {
namespace gpublur {

// 懒初始化（首次调用触发；线程：仅渲染线程使用，无锁）。false = 永久不可用。
bool init();
inline bool available() { return init(); }

// 两趟卷积（H 用 kx，V 用 ky），就地作用于 buf。
// buf：RGBA 预乘像素，W×H×4 字节；plane：单通道 alpha 平面，W×H 字节
//（平面以 uint4 的 .x 通道承载，其余通道恒 0）。
// 返回 true = GPU 完成且已写回；false = 调用方必须执行 CPU 等价路径。
bool blurRgba(std::uint8_t* buf, int W, int H,
              const std::vector<float>& kx, const std::vector<float>& ky);
bool blurPlane(std::uint8_t* buf, int W, int H,
               const std::vector<float>& kx, const std::vector<float>& ky);

}  // namespace gpublur
}  // namespace swf
