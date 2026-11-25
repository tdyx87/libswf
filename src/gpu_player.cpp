// gpu_player.cpp
// GPU 渲染适配器骨架的验证入口（可选构建：-DSWFPARSER_D3D11_BACKEND=ON）。
//
//   swfparser_gpu <file.swf> --render out.png [--frame N] [--scale S] [--click F:X:Y]...
//
// 流程与窗口播放器一致：Player 维护显示列表 -> GpuRenderSink 翻译成
// swfr::Renderer 命令 -> 离屏渲染 -> 读回 RGBA 写 PNG，可与 swfparser --render
// 的软件光栅输出直接比对。
//
// --frame N 与软件光栅器的 --render 完全同义：从装载状态播放 N 帧后出图
// （N=0 即未播放的装载状态）。两者都调 Player::advanceFrames，避免语义漂移。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "render/Renderer.h"
#include "swf/GpuRenderSink.hpp"
#include "swf/Player.hpp"

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

// stb_image_write 的实现已编译在 Render.cpp（swf_render）里。
extern "C" int stbi_write_png(char const* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

namespace {

// 离屏目标：MSAA 渲染 -> resolve -> staging 读回。
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
    if (FAILED(device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_R8G8B8A8_UNORM, samples, &quality)) || quality == 0) {
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

    // 遮罩（clipDepth / AVM2 mask）走 stencil，必须提供 DSV。
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

void Usage() {
    std::printf("usage: swfparser_gpu <file.swf> --render out.png [--frame N] [--scale S]\n");
    std::printf("  --frame N   play N frames from the loaded state, then render (same as swfparser), default 0\n");
    std::printf("  --click F:X:Y  press at step F, release at F+2 (same schedule as swfparser --play-game)\n");
    std::printf("  --range A:B --outdir D  export steps [A,B) as D/g_%%04d.png in one replay\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string outPath;
    int frame = 0;
    int scale = 1;
    // --click F:X:Y（可重复）：与 swfparser --play-game 同一套输入调度（F 帧按下、F+2 帧释放），
    // 用于把 GPU 送进需要交互才能到达的画面（游戏菜单、关卡 loading）。
    struct Click { int frame; double x; double y; };
    std::vector<Click> clicks;
    int rangeStart = 0;
    int rangeEnd = -1;          // >=0 时进入批量导出：step 区间 [rangeStart, rangeEnd) 各出一张 PNG
    std::string outDir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--render" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "--frame" && i + 1 < argc)
            frame = std::atoi(argv[++i]);
        else if (arg == "--scale" && i + 1 < argc)
            scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--click" && i + 1 < argc) {
            Click c{};
            if (std::sscanf(argv[++i], "%d:%lf:%lf", &c.frame, &c.x, &c.y) == 3)
                clicks.push_back(c);
        } else if (arg == "--range" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d:%d", &rangeStart, &rangeEnd) != 2) rangeEnd = -1;
        } else if (arg == "--outdir" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            Usage();
            return 0;
        } else
            path = arg;
    }

    if (path.empty() || (outPath.empty() && (outDir.empty() || rangeEnd < 0))) {
        Usage();
        return 1;
    }

    swf::Player player;
    std::string err;
    if (!player.loadFile(path.c_str(), err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }

    if (rangeEnd >= 0 && !outDir.empty()) {
        // 批量导出：一次装载 + 一次交互回放，逐 step 出图（与 --play-game 的 p_%04d.png 对齐，
        // step 与文件名同号）。逐帧模式要重放 N 次，扫一段动画时太慢。
        std::map<int, std::pair<double, double>> down, up;
        for (const Click& c : clicks) {
            down[c.frame] = { c.x, c.y };
            up[c.frame + 2] = { c.x, c.y };
        }

        const int width = std::max(1, player.stageW() * scale);
        const int height = std::max(1, player.stageH() * scale);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                     _countof(levels), D3D11_SDK_VERSION, &device, nullptr, &context))) {
            std::fprintf(stderr, "D3D11 device creation failed\n");
            return 1;
        }

        OffscreenTarget target;
        if (!CreateTarget(device.Get(), static_cast<UINT>(width), static_cast<UINT>(height), &target)) {
            std::fprintf(stderr, "render target creation failed\n");
            return 1;
        }

        swfr::Renderer renderer;
        if (!renderer.Initialize(device.Get(), context.Get())) {
            std::fprintf(stderr, "renderer initialization failed\n");
            return 1;
        }

        const swf::Color bg = player.backgroundColor();
        const float clear[4] = { bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f };
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);

        auto renderTo = [&](const std::string& out) -> bool {
            renderer.BeginFrame(target.rtv.Get(), target.dsv.Get(),
                                static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), clear);
            swf::GpuRenderSink sink(renderer);
            const bool ok = player.renderFrame(sink, width, height);
            renderer.EndFrame();
            // 每帧一个 sink：不释放的话，形状/纹理句柄会在批量导出里逐帧增长。
            sink.releaseGpuResources();
            if (!ok) return false;

            ID3D11Texture2D* resolved = target.msaaTexture.Get();
            if (target.samples > 1) {
                context->ResolveSubresource(target.resolveTexture.Get(), 0, target.msaaTexture.Get(), 0,
                                            DXGI_FORMAT_R8G8B8A8_UNORM);
                resolved = target.resolveTexture.Get();
            }
            context->CopyResource(target.staging.Get(), resolved);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            for (int y = 0; y < height; ++y)
                std::memcpy(&rgba[static_cast<std::size_t>(y) * width * 4],
                            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                            static_cast<std::size_t>(width) * 4);
            context->Unmap(target.staging.Get(), 0);
            return stbi_write_png(out.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
        };

        int rendered = 0;
        for (int step = 0; step < rangeEnd; ++step) {
            auto d = down.find(step);
            if (d != down.end()) {
                player.setMouse(d->second.first, d->second.second);
                player.mouseDown();
            }
            auto u = up.find(step);
            if (u != up.end()) {
                player.setMouse(u->second.first, u->second.second);
                player.mouseUp();
            }
            player.tick();
            if (step < rangeStart) continue;

            char name[512];
            std::snprintf(name, sizeof(name), "%s/g_%04d.png", outDir.c_str(), step);
            if (!renderTo(name)) {
                std::fprintf(stderr, "render/write failed at step %d (%s)\n", step, name);
                return 2;
            }
            ++rendered;
        }

        std::printf("exported %d frames [%d,%d) -> %s/\n", rendered, rangeStart, rangeEnd, outDir.c_str());
        renderer.Shutdown();
        return 0;
    }

    int advanced = frame;
    if (clicks.empty()) {
        advanced = player.advanceFrames(frame);
    } else {
        // 交互回放：逐帧应用与 swfparser --play-game 相同的按下/释放调度后再 tick。
        std::map<int, std::pair<double, double>> down, up;
        for (const Click& c : clicks) {
            down[c.frame] = { c.x, c.y };
            up[c.frame + 2] = { c.x, c.y };
        }
        // 注意：不能按 player.totalFrames() 夹取。AVM2 影片的根时间轴往往只有 2 帧，
        // 推进量是"步数"（脚本/时间轴自管），夹取会把 --frame 2000 变成 --frame 1，
        // 回放出的根本不是同一画面。
        if (advanced < 0) advanced = 0;
        for (int step = 0; step < advanced; ++step) {
            auto d = down.find(step);
            if (d != down.end()) {
                player.setMouse(d->second.first, d->second.second);
                player.mouseDown();
            }
            auto u = up.find(step);
            if (u != up.end()) {
                player.setMouse(u->second.first, u->second.second);
                player.mouseUp();
            }
            player.tick();
        }
    }
    if (advanced != frame) {
        std::fprintf(stderr, "warning: --frame %d exceeds the timeline, clamped to %d\n",
                     frame, advanced);
    }

    const int width = std::max(1, player.stageW() * scale);
    const int height = std::max(1, player.stageH() * scale);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                 _countof(levels), D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::fprintf(stderr, "D3D11 device creation failed\n");
        return 1;
    }

    OffscreenTarget target;
    if (!CreateTarget(device.Get(), static_cast<UINT>(width), static_cast<UINT>(height), &target)) {
        std::fprintf(stderr, "render target creation failed\n");
        return 1;
    }

    swfr::Renderer renderer;
    if (!renderer.Initialize(device.Get(), context.Get())) {
        std::fprintf(stderr, "renderer initialization failed\n");
        return 1;
    }

    const swf::Color bg = player.backgroundColor();
    const float clear[4] = { bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f };

    renderer.BeginFrame(target.rtv.Get(), target.dsv.Get(),
                        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), clear);

    swf::GpuRenderSink sink(renderer);
    const bool ok = player.renderFrame(sink, width, height);
    renderer.EndFrame();

    if (!ok) {
        std::fprintf(stderr,
                     "frame %d is not renderable through the GPU skeleton "
                     "(AVM2 runtime stage not adapted yet)\n", player.currentFrame());
        return 2;
    }

    // MSAA resolve -> staging -> 读回
    ID3D11Texture2D* resolved = target.msaaTexture.Get();
    if (target.samples > 1) {
        context->ResolveSubresource(target.resolveTexture.Get(), 0, target.msaaTexture.Get(), 0,
                                    DXGI_FORMAT_R8G8B8A8_UNORM);
        resolved = target.resolveTexture.Get();
    }
    context->CopyResource(target.staging.Get(), resolved);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        std::fprintf(stderr, "readback failed\n");
        return 1;
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(&rgba[static_cast<std::size_t>(y) * width * 4],
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(width) * 4);
    context->Unmap(target.staging.Get(), 0);

    if (!stbi_write_png(outPath.c_str(), width, height, 4, rgba.data(), width * 4)) {
        std::fprintf(stderr, "write png failed: %s\n", outPath.c_str());
        return 1;
    }

    const swf::GpuRenderSink::Stats& stats = sink.stats();
    const swfr::Stats& gpu = renderer.GetStats();
    std::printf("frame %d (index %d) -> %s (%dx%d, msaa x%u)\n", player.currentFrame(),
                player.currentFrame() - 1, outPath.c_str(), width, height, target.samples);
    std::printf("  gpu: draws=%u tris=%u masks=%u layers=%u blurPasses=%u\n", gpu.drawCalls,
                gpu.triangles, gpu.masks, gpu.layers, gpu.blurPasses);
    std::printf("  sink: fills=%llu strokes=%llu glyphs=%llu bitmaps=%llu editTexts=%llu "
                "masks=%llu maskGroups=%llu filterLayers=%llu blurs=%llu shadows=%llu "
                "filtersDropped=%llu blendFallback=%llu\n",
                (unsigned long long)stats.fills, (unsigned long long)stats.strokes,
                (unsigned long long)stats.glyphs, (unsigned long long)stats.bitmaps,
                (unsigned long long)stats.editTexts, (unsigned long long)stats.masks,
                (unsigned long long)stats.maskGroups, (unsigned long long)stats.filterLayers,
                (unsigned long long)stats.blurs, (unsigned long long)stats.shadows,
                (unsigned long long)stats.filtersDropped,
                (unsigned long long)stats.blendFallback);
    std::printf("  editText: drawnGlyphs=%llu deviceFontSkipped=%llu\n",
                (unsigned long long)stats.editTextGlyphs,
                (unsigned long long)stats.editTextDeviceGlyphs);
    std::printf("  cache: shapes=%zu textures=%zu\n", sink.cachedShapeCount(), sink.cachedTextureCount());

    sink.releaseGpuResources();
    renderer.Shutdown();
    return 0;
}
