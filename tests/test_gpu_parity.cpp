// 软件光栅器 vs D3D11 适配器的逐像素一致性回归测试（CTest: swfparser_gpu_parity）。
//
// 同一 SWF、同一帧，分别经：
//   * 软件路径：Player::render（Render.cpp 的 CPU 光栅化）
//   * GPU 路径：Player::renderFrame(GpuRenderSink) -> swfr::Renderer（../d3drender）
// 渲染到 RGBA 后逐像素比较。两条路径本应几乎一致，差异只允许来自抗锯齿实现
// （软件 2x2 超采样 vs GPU 4x MSAA）与模糊的舍入。因此判定标准是：
//   * 尺寸必须一致；
//   * 平均通道差 <= kMaxMean；
//   * 超过 kOutlierDelta 的像素占比 <= kMaxOutlierRatio。
// 任何"整块没画/画错位置/颜色错/滤镜丢失"都会让大面积像素越界，从而失败。
//
// 无可用 D3D11 设备时返回 77（CTest 的 SKIP_RETURN_CODE），而不是假装通过。
#include <d3d11.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "render/Renderer.h"
#include "swf/GpuRenderSink.hpp"
#include "swf/Player.hpp"

extern "C" int stbi_write_png(char const* filename, int w, int h, int comp, const void* data, int stride_bytes);

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

#ifndef SWF_TEST_DATA_DIR
#define SWF_TEST_DATA_DIR "testswf"
#endif

namespace {

constexpr int kSkipReturnCode = 77;
// 抗锯齿/模糊舍入容差：单像素超过 24 灰阶视为"结构性差异"。
constexpr int kOutlierDelta = 24;
// 结构性差异像素占比上限（0.5%）：只允许出现在形状边缘。
constexpr double kMaxOutlierRatio = 0.005;
constexpr double kMaxMean = 2.5;

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 离屏目标：MSAA 渲染 -> resolve -> staging 读回（与 swfparser_gpu 工具同构）。
struct OffscreenTarget {
    ComPtr<ID3D11Texture2D> msaaTexture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Texture2D> resolveTexture;
    ComPtr<ID3D11Texture2D> staging;
    UINT width = 0;
    UINT height = 0;
    UINT samples = 1;
};

bool CreateTarget(ID3D11Device* device, UINT width, UINT height, OffscreenTarget* out) {
    UINT quality = 0;
    UINT samples = 4;
    if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &quality)) ||
        quality == 0) {
        samples = 1;
    }

    D3D11_TEXTURE2D_DESC colorDesc{};
    colorDesc.Width = width;
    colorDesc.Height = height;
    colorDesc.MipLevels = 1;
    colorDesc.ArraySize = 1;
    colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colorDesc.SampleDesc.Count = samples;
    colorDesc.Usage = D3D11_USAGE_DEFAULT;
    colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    if (FAILED(device->CreateTexture2D(&colorDesc, nullptr, &out->msaaTexture))) return false;
    if (FAILED(device->CreateRenderTargetView(out->msaaTexture.Get(), nullptr, &out->rtv))) return false;

    D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, &out->depthTexture))) return false;
    if (FAILED(device->CreateDepthStencilView(out->depthTexture.Get(), nullptr, &out->dsv))) return false;

    if (samples > 1) {
        colorDesc.SampleDesc.Count = 1;
        if (FAILED(device->CreateTexture2D(&colorDesc, nullptr, &out->resolveTexture))) return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = colorDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &out->staging))) return false;

    out->width = width;
    out->height = height;
    out->samples = samples;
    return true;
}

bool ReadBack(ID3D11DeviceContext* context, OffscreenTarget* target,
              std::vector<std::uint8_t>* out) {
    ID3D11Texture2D* resolved = target->msaaTexture.Get();
    if (target->samples > 1) {
        context->ResolveSubresource(target->resolveTexture.Get(), 0, target->msaaTexture.Get(), 0,
                                    DXGI_FORMAT_R8G8B8A8_UNORM);
        resolved = target->resolveTexture.Get();
    }
    context->CopyResource(target->staging.Get(), resolved);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(target->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    const UINT width = target->width;
    const UINT height = target->height;
    out->assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(&(*out)[static_cast<std::size_t>(y) * width * 4],
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(width) * 4);
    }
    context->Unmap(target->staging.Get(), 0);
    return true;
}

bool RenderGpu(ID3D11Device* device, ID3D11DeviceContext* context, swf::Player& player,
               int width, int height, std::vector<std::uint8_t>* out) {
    OffscreenTarget target;
    if (!CreateTarget(device, static_cast<UINT>(width), static_cast<UINT>(height), &target))
        return false;

    swfr::Renderer renderer;
    if (!renderer.Initialize(device, context))
        return false;

    const swf::Color bg = player.backgroundColor();
    const float clear[4] = { bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f };
    renderer.BeginFrame(target.rtv.Get(), target.dsv.Get(), static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height), clear);

    swf::GpuRenderSink sink(renderer);
    const bool ok = player.renderFrame(sink, width, height);
    renderer.EndFrame();
    sink.releaseGpuResources();
    renderer.Shutdown();
    if (!ok)
        return false;

    return ReadBack(context, &target, out);
}

// ---- 复用同一份 Renderer/GpuRenderSink 连续渲染多帧 ----
// 窗口播放器就是这样跑的：sink 与它的网格缓存跨帧存活。每帧新建 sink 的用例
// 永远发现不了「缓存按 Shape 地址命中旧网格」这类冻结（逐帧插值的 Morph 填充、
// 逐帧平移的精灵遮罩都是每帧新建 Shape，分配器复用地址）。
struct PersistentGpu {
    OffscreenTarget target;
    swfr::Renderer renderer;
    std::unique_ptr<swf::GpuRenderSink> sink;
};

bool InitPersistentGpu(ID3D11Device* device, ID3D11DeviceContext* context, UINT width,
                       UINT height, PersistentGpu* g) {
    if (!CreateTarget(device, width, height, &g->target)) return false;
    if (!g->renderer.Initialize(device, context)) return false;
    g->sink = std::make_unique<swf::GpuRenderSink>(g->renderer);
    return true;
}

bool RenderPersistentGpu(PersistentGpu& g, ID3D11DeviceContext* context, swf::Player& player,
                         int width, int height, std::vector<std::uint8_t>* out) {
    const swf::Color bg = player.backgroundColor();
    const float clear[4] = { bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f };
    g.renderer.BeginFrame(g.target.rtv.Get(), g.target.dsv.Get(),
                          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                          clear);
    const bool ok = player.renderFrame(*g.sink, width, height);
    g.renderer.EndFrame();
    if (!ok) return false;
    return ReadBack(context, &g.target, out);
}

// 把 CPU/GPU 两图的结构性差异画成一张对比图（差异灰阶 x8 放大），
// 一致性失败时直接落盘，肉眼就能看出是整块没画、画错位置还是边缘抗锯齿。
bool WriteDiffPng(const std::string& path, const std::vector<std::uint8_t>& software,
                  const std::vector<std::uint8_t>& gpu, int width, int height) {
    if (software.size() != gpu.size()) return false;
    std::vector<std::uint8_t> diff(software.size());
    for (std::size_t p = 0; p < software.size(); p += 4) {
        int d = 0;
        for (int c = 0; c < 4; ++c)
            d = std::max(d, std::abs(static_cast<int>(software[p + c]) -
                                     static_cast<int>(gpu[p + c])));
        const std::uint8_t v = static_cast<std::uint8_t>(std::min(255, d * 8));
        diff[p + 0] = v;
        diff[p + 1] = v;
        diff[p + 2] = v;
        diff[p + 3] = 255;
    }
    return stbi_write_png(path.c_str(), width, height, 4, diff.data(), width * 4) != 0;
}

// 逐像素比较；返回是否在容差内，同时打印诊断。
bool Compare(const std::vector<std::uint8_t>& software, const std::vector<std::uint8_t>& gpu,
             int width, int height) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (software.size() != pixels * 4 || gpu.size() != pixels * 4) {
        std::printf("    buffer size mismatch (sw=%zu gpu=%zu expected=%zu)\n",
                    software.size(), gpu.size(), pixels * 4);
        return false;
    }

    std::size_t outliers = 0;
    int maxDelta = 0;
    double sum = 0.0;
    for (std::size_t p = 0; p < pixels; ++p) {
        int delta = 0;
        for (int c = 0; c < 4; ++c) {
            const int a = software[p * 4 + c];
            const int b = gpu[p * 4 + c];
            delta = std::max(delta, std::abs(a - b));
        }
        sum += delta;
        if (delta > kOutlierDelta) ++outliers;
        maxDelta = std::max(maxDelta, delta);
    }

    const double mean = sum / static_cast<double>(pixels);
    const double ratio = static_cast<double>(outliers) / static_cast<double>(pixels);
    std::printf("    %dx%d mean=%.3f max=%d outliers=%zu (%.3f%%)\n", width, height, mean, maxDelta,
                outliers, ratio * 100.0);

    const bool ok = ratio <= kMaxOutlierRatio && mean <= kMaxMean;
    if (!ok) {
        std::printf("    tolerance: mean<=%.2f, outliers<=%.3f%%\n", kMaxMean,
                    kMaxOutlierRatio * 100.0);
    }
    return ok;
}

struct Case {
    const char* file;
    int frame;
    bool optional = false;   // 文件缺失时跳过（真实影片样本不在仓库里）
};

// full_test.swf 不在此列：它的位图填充写成 0.08 像素的平铺周期（生成器把
// twips/像素写反了），软件端每像素只采一个纹素、GPU 无 mipmap 采样出摩尔纹，
// 属已知的纹理抗锯齿差异，不是适配器回归。
const Case kCases[] = {
    { "shape_test.swf", 0 },
    { "gradient_test.swf", 0 },
    { "bitmap_fill_test.swf", 0 },
    { "filter_test.swf", 0 },
    // 一个裁剪组里多条遮罩形状（并集）：GPU 必须两条都进模板，而不是只取第一条。
    { "mask_union_test.swf", 0 },
    // 真实 AVM2 影片（运行时显示树）：覆盖动态文本/遮罩/滤镜/混合模式等
    // 只有运行时渲染路径才会走到的分支。样本不随仓库分发，缺失时跳过。
    { "../../swf2.swf", 1, true },
};

}  // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                 _countof(levels), D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::printf("SKIP: D3D11 device creation failed (no hardware GPU)\n");
        return kSkipReturnCode;
    }

    for (const Case& c : kCases) {
        const std::string path = std::string(SWF_TEST_DATA_DIR) + "/" + c.file;
        std::printf("%s frame %d\n", c.file, c.frame);

        if (c.optional) {
            std::FILE* probe = std::fopen(path.c_str(), "rb");
            if (!probe) {
                std::printf("  [SKIP] sample not found: %s\n", path.c_str());
                continue;
            }
            std::fclose(probe);
        }

        swf::Player player;
        std::string err;
        if (!player.loadFile(path.c_str(), err)) {
            std::printf("  [FAIL] load %s: %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        // 与两个 CLI 的 --frame 走同一个入口：帧号语义只有一份定义。
        player.advanceFrames(c.frame);

        std::vector<std::uint8_t> software;
        int width = 0, height = 0;
        if (!player.render(software, width, height)) {
            Check(false, "software render");
            continue;
        }

        std::vector<std::uint8_t> gpu;
        if (!RenderGpu(device.Get(), context.Get(), player, width, height, &gpu)) {
            Check(false, "gpu render");
            continue;
        }

        const bool ok = Compare(software, gpu, width, height);
        if (!ok) {
            // 失败时落盘一对图，方便直接看出是哪块内容对不上。
            std::string name = c.file;
            for (char& ch : name) if (ch == '/' || ch == '\\' || ch == '.') ch = '_';
            const std::string base = "build/parity_" + name;
            stbi_write_png((base + "_cpu.png").c_str(), width, height, 4, software.data(), width * 4);
            stbi_write_png((base + "_gpu.png").c_str(), width, height, 4, gpu.data(), width * 4);
            std::printf("    dumped %s_cpu.png / %s_gpu.png\n", base.c_str(), base.c_str());
        }
        Check(ok, "software == gpu within tolerance");
    }

    // ---- 帧序列回归：同一种场景每帧各存一张 CPU 图和一张 GPU 图，直接对比 ----
    // 两个样本各 5 帧，逐帧推进同一个播放器，并复用同一份 Renderer/GpuRenderSink
    // （窗口播放器就是这样跑的：网格缓存跨帧存活）。每帧都落盘
    // build/parity_seq/<场景>_f<N>_{cpu,gpu}.png，然后逐像素比较这两张图。
    // 缓存若只按 Shape 地址命中，GPU 的第 2..5 帧会停在第 1 帧：图上肉眼可见
    // （"数字在走、条不动"），逐像素比较则会让 CI 直接红。
    {
        const char* kSequences[] = { "morph_anim_test.swf", "mask_anim_test.swf" };
        constexpr int kSeqFrames = 5;
        // 固定落在工程根的 build/parity_seq（不随 ctest 的工作目录变化）。
        const std::filesystem::path dataDir = SWF_TEST_DATA_DIR;
        const std::string dumpDir =
            (dataDir.parent_path() / "build" / "parity_seq").string();
        std::error_code dirEc;
        std::filesystem::create_directories(dumpDir, dirEc);

        for (const char* file : kSequences) {
            const std::string path = std::string(SWF_TEST_DATA_DIR) + "/" + file;
            std::string stem = file;
            const std::size_t dot = stem.rfind('.');
            if (dot != std::string::npos) stem.resize(dot);
            std::printf("%s frames 1..%d (sink reused, images -> %s/)\n", file, kSeqFrames,
                        dumpDir.c_str());

            swf::Player player;
            std::string err;
            if (!player.loadFile(path.c_str(), err)) {
                std::printf("  [FAIL] load %s: %s\n", path.c_str(), err.c_str());
                ++g_fail;
                continue;
            }

            std::vector<std::uint8_t> software, gpu;
            int width = 0, height = 0;
            if (!player.render(software, width, height)) {
                Check(false, "sequence software render");
                continue;
            }

            PersistentGpu persistent;
            if (!InitPersistentGpu(device.Get(), context.Get(), static_cast<UINT>(width),
                                   static_cast<UINT>(height), &persistent)) {
                Check(false, "sequence gpu init");
                continue;
            }

            bool parity = true;
            for (int f = 1; f <= kSeqFrames; ++f) {
                player.advanceFrames(1);
                if (!player.render(software, width, height) ||
                    !RenderPersistentGpu(persistent, context.Get(), player, width, height, &gpu)) {
                    parity = false;
                    break;
                }
                const std::string base =
                    dumpDir + "/" + stem + "_f" + std::to_string(f);
                stbi_write_png((base + "_cpu.png").c_str(), width, height, 4, software.data(),
                               width * 4);
                stbi_write_png((base + "_gpu.png").c_str(), width, height, 4, gpu.data(),
                               width * 4);
                std::printf("  frame %d  %s_{cpu,gpu}.png\n", f, base.c_str());
                if (!Compare(software, gpu, width, height)) {
                    parity = false;
                    WriteDiffPng(base + "_diff.png", software, gpu, width, height);
                    std::printf("    dumped %s_diff.png\n", base.c_str());
                }
            }

            persistent.sink->releaseGpuResources();
            persistent.renderer.Shutdown();
            const std::string what = std::string("sequence: cpu == gpu (") + file +
                                     ", sink reused)";
            Check(parity, what.c_str());
        }
    }

    if (g_fail == 0)
        std::printf("\nall gpu parity cases passed\n");
    return g_fail == 0 ? 0 : 1;
}
