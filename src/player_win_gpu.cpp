// swfplayer_gpu —— D3D11 交换链窗口播放器。
//
// 与 swfplayer（Win32 + GDI + 软件光栅器）行为对齐：按影片帧率推进时间轴、
// 执行 AVM1/AVM2 脚本、鼠标/键盘事件转发、舞台按 Show All 等比缩放居中。
// 区别是每一帧都经 IDisplayListSink -> swfr::Renderer 直接画进交换链后台缓冲：
// 形状填充/描边/字形/位图/遮罩/滤镜全部由 GPU 后端完成，不再经过 CPU 像素缓冲，
// 也没有 RGBA->BGRA 转换与 StretchDIBits 缩放（矢量/文字随窗口尺寸保持清晰）。
//
// 命令行：swfplayer_gpu <file.swf> [--pause] [--click F:X:Y]...
//   --pause        首帧定格，等待播放（目前无播放键，主要用于看首帧）
//   --click F:X:Y  第 F 次 tick 在舞台坐标 (X,Y) 注入一次左键点击（按 2 帧后释放）
//
// 渲染全部通过 GPU（IDisplayListSink -> GpuRenderSink -> D3D11 渲染器）完成，
// 包含传统显示列表与 AVM2 运行时舞台内容；窗口标题实时显示 FPS。
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "render/Renderer.h"
#include "swf/GpuRenderSink.hpp"
#include "swf/Player.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

swf::Player g_player;
swf::GpuRenderSink* g_sink = nullptr;   // 会话级：形状/纹理缓存跨帧复用
swfr::Renderer g_renderer;

HWND g_hwnd = nullptr;
int g_stageW = 550, g_stageH = 400;
bool g_playing = true;
bool g_warnedRuntimeStage = false;
// 暂停/停止状态下也要画面：主循环在 !g_playing 时只等消息，必须补画一帧，
// 否则 --pause 启动或停下后窗口一片白（交换链从未 Present 过）。
bool g_needFrame = true;
long long g_tickCount = 0;   // 单调 tick 计数（--click 的时基）
std::map<int, std::pair<double, double>> g_clickDown, g_clickUp;
std::wstring g_baseTitle;

// 心跳统计（每 24 帧打印一行，确认“真的在画”而不只是窗口开着）。
long long g_hbDraws = 0, g_hbTris = 0, g_hbLayers = 0, g_hbFrames = 0;
// 分段耗时（微秒），定位卡顿来源：脚本 tick / 遍历+提交 / Present。
double g_hbTickUs = 0.0, g_hbRenderUs = 0.0, g_hbPresentUs = 0.0;
LARGE_INTEGER g_fpsFreq{}, g_fpsT0{};
long long g_fpsFrames = 0;
double g_fpsValue = 0.0;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11Texture2D> g_backBuffer;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11Texture2D> g_depthTexture;
ComPtr<ID3D11DepthStencilView> g_dsv;

// 多重采样：窗口直画交换链背面时边缘只有单采样，角色/文字边缘会明显发毛。
// 这里先画进 MSAA 离屏目标，再 Resolve 到背面（与 CLI 导出走同一套画质）。
ComPtr<ID3D11Texture2D> g_msaaColor;
ComPtr<ID3D11RenderTargetView> g_msaaRtv;
UINT g_sampleCount = 1;



// ---- 设备 / 交换链 ----

bool CreateDeviceAndSwapChain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, _countof(levels),
        D3D11_SDK_VERSION, &sd, &g_swap, &g_device, nullptr, &g_context);
    if (FAILED(hr)) {
        // 无硬件 GPU（虚拟机/远程桌面）时退回 WARP：D3D11 参考光栅化器同样可用。
        g_swap.Reset();
        g_device.Reset();
        g_context.Reset();
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, _countof(levels),
            D3D11_SDK_VERSION, &sd, &g_swap, &g_device, nullptr, &g_context);
    }
    if (FAILED(hr)) {
        std::printf("[gpu-player] D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)\n",
                    static_cast<unsigned>(hr));
        return false;
    }
    // 诊断：打印实际使用的适配器（WARP = CPU 软件光栅化，性能会非常差）。
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(g_device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&ad))) {
            char name[256] = { 0 };
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            std::printf("[gpu-player] adapter: %s  vendor=0x%04X device=0x%04X vram=%lluMB\n",
                        name, ad.VendorId, ad.DeviceId,
                        static_cast<unsigned long long>(ad.DedicatedVideoMemory / (1024ull * 1024ull)));
            std::fflush(stdout);
        }
    }
    return true;
}

void ReleaseBackBufferViews() {
    if (g_context) {
        ID3D11RenderTargetView* none[1] = { nullptr };
        g_context->OMSetRenderTargets(1, none, nullptr);
    }
    g_msaaRtv.Reset();
    g_msaaColor.Reset();
    g_backBuffer.Reset();
    g_dsv.Reset();
    g_depthTexture.Reset();
    g_rtv.Reset();
}

bool CreateBackBufferViews(int width, int height) {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                 reinterpret_cast<void**>(backBuffer.GetAddressOf()))))
        return false;
    if (FAILED(g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, g_rtv.GetAddressOf())))
        return false;
    g_backBuffer = backBuffer;

    // 取设备支持的最高 MSAA 档（8 -> 4 -> 2）；都不支持就退回单采样直画。
    g_sampleCount = 1;
    const UINT wants[] = { 8u, 4u, 2u };
    for (UINT want : wants) {
        UINT levels = 0;
        if (SUCCEEDED(g_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, want,
                                                              &levels)) &&
            levels > 0) {
            g_sampleCount = want;
            break;
        }
    }
    if (g_sampleCount > 1) {
        D3D11_TEXTURE2D_DESC colorDesc{};
        colorDesc.Width = static_cast<UINT>(width);
        colorDesc.Height = static_cast<UINT>(height);
        colorDesc.MipLevels = 1;
        colorDesc.ArraySize = 1;
        colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorDesc.SampleDesc.Count = g_sampleCount;
        colorDesc.Usage = D3D11_USAGE_DEFAULT;
        colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&colorDesc, nullptr, g_msaaColor.GetAddressOf())) ||
            FAILED(g_device->CreateRenderTargetView(g_msaaColor.Get(), nullptr,
                                                    g_msaaRtv.GetAddressOf())))
            return false;
    }

    // 遮罩（clipDepth / filter 层）走模板缓冲，必须提供 DSV；采样数要与颜色目标一致。
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = static_cast<UINT>(width);
    depthDesc.Height = static_cast<UINT>(height);
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = g_sampleCount;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_device->CreateTexture2D(&depthDesc, nullptr, g_depthTexture.GetAddressOf())) ||
        FAILED(g_device->CreateDepthStencilView(g_depthTexture.Get(), nullptr, g_dsv.GetAddressOf())))
        return false;
    return true;
}

bool ResizeSwapChain(int width, int height) {
    if (!g_swap || width < 1 || height < 1) return false;
    ReleaseBackBufferViews();
    if (FAILED(g_swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                     DXGI_FORMAT_UNKNOWN, 0)))
        return false;
    return CreateBackBufferViews(width, height);
}

// Adobe Flash Player 的默认缩放模式是 Show All：舞台等比缩放后在客户区居中。
// 渲染内容尺寸 = 该视口尺寸（矢量/文字按新分辨率重画），偏移交给
// Renderer::SetOutputRect（letterbox 区域在 BeginFrame 里清黑）。
void StageViewport(int clientW, int clientH, int& w, int& h, int& ox, int& oy) {
    if (clientW < 1) clientW = 1;
    if (clientH < 1) clientH = 1;
    const double kx = static_cast<double>(clientW) / g_stageW;
    const double ky = static_cast<double>(clientH) / g_stageH;
    const double k = kx < ky ? kx : ky;
    w = static_cast<int>(g_stageW * k + 0.5);
    h = static_cast<int>(g_stageH * k + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ox = (clientW - w) / 2;
    oy = (clientH - h) / 2;
}

// 客户区像素 -> 舞台坐标（必须与渲染共用同一视口，放大/留黑边后点击才不偏移）。
void ClientToStage(int cx, int cy, int clientW, int clientH, double& sx, double& sy) {
    int vw = 0, vh = 0, ox = 0, oy = 0;
    StageViewport(clientW, clientH, vw, vh, ox, oy);
    sx = (cx - ox) * (static_cast<double>(g_stageW) / vw);
    sy = (cy - oy) * (static_cast<double>(g_stageH) / vh);
}

void UpdateCursor() {
    SetCursor(LoadCursor(nullptr, g_player.handCursor() ? IDC_HAND : IDC_ARROW));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // 内容由交换链呈现，禁止擦背景防闪烁

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                const int w = LOWORD(lp);
                const int h = HIWORD(lp);
                if (w > 0 && h > 0) ResizeSwapChain(w, h);
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                UpdateCursor();
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            double sx, sy;
            ClientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), rc.right - rc.left,
                          rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            UpdateCursor();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            double sx, sy;
            ClientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), rc.right - rc.left,
                          rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            g_player.mouseDown();
            return 0;
        }

        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            double sx, sy;
            ClientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), rc.right - rc.left,
                          rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            g_player.mouseUp();
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            g_player.keyDown(static_cast<int>(wp));
            return 0;

        case WM_KEYUP:
            g_player.keyUp(static_cast<int>(wp));
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// 一帧：全部通过 GPU 渲染（D3D11 适配器/sink），不使用 CPU 软件光栅回退。
bool RenderFrame() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const int clientW = rc.right - rc.left > 0 ? rc.right - rc.left : 1;
    const int clientH = rc.bottom - rc.top > 0 ? rc.bottom - rc.top : 1;

    int vw = 0, vh = 0, ox = 0, oy = 0;
    StageViewport(clientW, clientH, vw, vh, ox, oy);

    g_renderer.SetOutputRect(ox, oy, static_cast<uint32_t>(clientW),
                             static_cast<uint32_t>(clientH));

    const swf::Color bg = g_player.backgroundColor();
    const float clear[4] = { bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, bg.a / 255.0f };
    g_renderer.BeginFrame(g_sampleCount > 1 ? g_msaaRtv.Get() : g_rtv.Get(), g_dsv.Get(),
                          static_cast<uint32_t>(vw), static_cast<uint32_t>(vh), clear);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool ok = g_player.renderFrame(*g_sink, vw, vh);
    g_renderer.EndFrame();
    QueryPerformanceCounter(&t1);
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_hbRenderUs += (t1.QuadPart - t0.QuadPart) * 1000000.0 / f.QuadPart;
    }

    if (!ok && !g_warnedRuntimeStage) {
        std::printf("[gpu-player] frame not rendered by GPU sink (showing background)\n");
        g_warnedRuntimeStage = true;
    }

    LARGE_INTEGER t2, t3;
    QueryPerformanceCounter(&t2);
    // MSAA 目标 -> 交换链背面（ResolveSubresource 要求源为多重采样、目标单采样）
    if (g_sampleCount > 1 && g_backBuffer && g_msaaColor)
        g_context->ResolveSubresource(g_backBuffer.Get(), 0, g_msaaColor.Get(), 0,
                                      DXGI_FORMAT_R8G8B8A8_UNORM);
    g_swap->Present(1, 0);
    QueryPerformanceCounter(&t3);
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_hbPresentUs += (t3.QuadPart - t2.QuadPart) * 1000000.0 / f.QuadPart;
    }

    // 诊断：累计自上次心跳以来的绘制量（每帧 BeginFrame 会清零 renderer 统计）。
    const swfr::Stats& st = g_renderer.GetStats();
    g_hbDraws += st.drawCalls;
    g_hbTris += st.triangles;
    g_hbLayers += st.layers;
    ++g_hbFrames;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // 不声明 DPI 感知时 Windows 会把整个窗口做位图虚拟化拉伸，最终显示发虚。
    if (HMODULE u32 = LoadLibraryW(L"user32.dll")) {
        using SetDPIAwareFn = BOOL(WINAPI*)();
        auto fn = reinterpret_cast<SetDPIAwareFn>(GetProcAddress(u32, "SetProcessDPIAware"));
        if (fn) fn();
        FreeLibrary(u32);
    }

    // 与 swfplayer 一致：作为 GUI 播放器使用时不保留控制台窗口（心跳输出将被丢弃）。
    FreeConsole();

    std::string path;
    bool startPaused = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pause") {
            startPaused = true;
        } else if (a == "--click" && i + 1 < argc) {
            int fr = 0;
            double x = 0, y = 0;
            if (std::sscanf(argv[++i], "%d:%lf:%lf", &fr, &x, &y) == 3) {
                g_clickDown[fr] = { x, y };
                g_clickUp[fr + 2] = { x, y };   // 按住 2 帧后释放
            }
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: swfplayer_gpu <file.swf> [--pause] [--click F:X:Y]...\n");
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            path = a;
        }
    }
    if (path.empty()) {
        std::printf("usage: swfplayer_gpu <file.swf> [--pause] [--click F:X:Y]...\n");
        return 1;
    }

    std::string err;
    if (!g_player.loadFile(path.c_str(), err)) {
        std::printf("[gpu-player] load FAILED: %s\n", err.c_str());
        MessageBoxA(nullptr, ("无法打开 " + path + "\n" + err).c_str(), "swfplayer_gpu",
                    MB_ICONERROR | MB_OK);
        return 1;
    }
    g_stageW = g_player.stageW() > 0 ? g_player.stageW() : 550;
    g_stageH = g_player.stageH() > 0 ? g_player.stageH() : 400;
    g_playing = !startPaused;
    if (!g_playing) {
        // 暂停态也要先让首帧的显示列表/帧脚本就位，否则窗口一片黑。
        {
            LARGE_INTEGER a, b, f;
            QueryPerformanceCounter(&a);
            g_player.tick();
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            g_hbTickUs += (b.QuadPart - a.QuadPart) * 1000000.0 / f.QuadPart;
        }
        g_player.stop();
    }
    std::printf("[gpu-player] %s  stage=%dx%d frames=%d fps=%.1f vms=%zu\n", path.c_str(),
                g_stageW, g_stageH, g_player.totalFrames(), g_player.frameRate(),
                g_player.vmCount());

    // 舞台可能远大于屏幕（如 25600x15360 twips），等比缩小到可用桌面区域。
    const int maxW = GetSystemMetrics(SM_CXSCREEN) - 40;
    const int maxH = GetSystemMetrics(SM_CYSCREEN) - 80;
    double scale = 1.0;
    if (g_stageW > maxW) scale = static_cast<double>(maxW) / g_stageW;
    if (g_stageH * scale > maxH) scale = static_cast<double>(maxH) / g_stageH;
    int clientW = std::max(1, static_cast<int>(g_stageW * scale + 0.5));
    int clientH = std::max(1, static_cast<int>(g_stageH * scale + 0.5));

    HINSTANCE hInst = GetModuleHandle(nullptr);
    const wchar_t* kClass = L"swfplayer_gpu_wnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    RECT wr{ 0, 0, clientW, clientH };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, kClass, L"swfplayer (D3D11)", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left,
                             wr.bottom - wr.top, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        std::printf("[gpu-player] CreateWindow failed\n");
        return 1;
    }

    RECT crc;
    GetClientRect(g_hwnd, &crc);
    if (!CreateDeviceAndSwapChain(g_hwnd) ||
        !CreateBackBufferViews(crc.right - crc.left, crc.bottom - crc.top)) {
        MessageBoxA(nullptr, "D3D11 初始化失败", "swfplayer_gpu", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!g_renderer.Initialize(g_device.Get(), g_context.Get())) {
        MessageBoxA(nullptr, "GPU 渲染器初始化失败", "swfplayer_gpu", MB_ICONERROR | MB_OK);
        return 1;
    }

    std::wstring wtitle;
    for (char c : path) wtitle.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    const size_t slash = wtitle.find_last_of(L"\\/");
    if (slash != std::wstring::npos) wtitle = wtitle.substr(slash + 1);
    wtitle += L" - swfplayer (D3D11)";
    g_baseTitle = wtitle;
    SetWindowTextW(g_hwnd, wtitle.c_str());

    swf::GpuRenderSink sink(g_renderer);
    g_sink = &sink;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    std::fflush(stdout);
    int fps = static_cast<int>(g_player.frameRate() + 0.5f);
    if (fps <= 0) fps = 12;
    // 与 swfplayer 相同的调度：WM_TIMER 粒度太粗（55ms 会被量化成 ~62.5ms），
    // 改用 QPC + MsgWaitForMultipleObjects 精确按影片帧率推进。
    timeBeginPeriod(1);
    LARGE_INTEGER freq, base;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&base);
    long long frameIdx = 0;
    const long long frameUs = 1000000LL / fps;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_sink = nullptr;
                sink.releaseGpuResources();
                g_renderer.Shutdown();
                timeEndPeriod(1);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g_playing) {
            if (g_needFrame) {
                g_needFrame = false;
                RenderFrame();
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            QueryPerformanceCounter(&base);
            frameIdx = 0;
            continue;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const long long elapsedUs = (now.QuadPart - base.QuadPart) * 1000000 / freq.QuadPart;
        const long long targetUs = frameIdx * frameUs;
        if (elapsedUs < targetUs) {
            long long waitMs = (targetUs - elapsedUs) / 1000;
            if (waitMs < 1) waitMs = 1;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(waitMs), QS_ALLINPUT);
            continue;
        }

        {
            LARGE_INTEGER a, b, f;
            QueryPerformanceCounter(&a);
            g_player.tick();
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            g_hbTickUs += (b.QuadPart - a.QuadPart) * 1000000.0 / f.QuadPart;
        }
        // 自动点击调度：按下在 tick F，释放在 tick F+2。
        if (auto dn = g_clickDown.find(static_cast<int>(g_tickCount)); dn != g_clickDown.end()) {
            g_player.setMouse(dn->second.first, dn->second.second);
            g_player.mouseDown();
        }
        if (auto up = g_clickUp.find(static_cast<int>(g_tickCount)); up != g_clickUp.end()) {
            g_player.setMouse(up->second.first, up->second.second);
            g_player.mouseUp();
        }

        RenderFrame();

        // 形状/纹理缓存以 Shape*/Bitmap* 为键：morph 的插值形状是每帧新建的临时
        // 对象，长期播放会让缓存无限增长（且含失效指针）。超过阈值整体重建一次。
        if (sink.cachedShapeCount() > 4000 || sink.cachedTextureCount() > 512)
            sink.releaseGpuResources();

        // FPS 统计与窗口标题展示（每 ~0.5s 更新一次）
    ++g_fpsFrames;
    {
        LARGE_INTEGER nowFps;
        QueryPerformanceCounter(&nowFps);
        if (g_fpsFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&g_fpsFreq);
            g_fpsT0 = nowFps;
        }
        const long long us = (nowFps.QuadPart - g_fpsT0.QuadPart) * 1000000 / g_fpsFreq.QuadPart;
        if (us >= 500000) {
            g_fpsValue = g_fpsFrames * 1000000.0 / us;
            g_fpsFrames = 0;
            g_fpsT0 = nowFps;
            if (g_hwnd && !g_baseTitle.empty()) {
                wchar_t title[256];
                std::swprintf(title, 256, L"%s  |  %.1f FPS", g_baseTitle.c_str(), g_fpsValue);
                SetWindowTextW(g_hwnd, title);
            }
        }
    }

    if (g_hbFrames >= 24) {
            std::printf("[gpu-player] frame=%d tick=%lld draws=%lld tris=%lld layers=%lld fps=%.1f\n",
                        g_player.currentFrame(), g_tickCount + 1, g_hbDraws, g_hbTris, g_hbLayers, g_fpsValue);
            const double n = static_cast<double>(g_hbFrames);
            std::printf("[gpu-player]   tick=%.1fms render=%.1fms present=%.1fms "
                        "fills=%llu strokes=%llu glyphs=%llu bmp=%llu edit=%llu "
                        "shapes=%llu masks=%llu filterLayers=%llu blurs=%llu shadows=%llu dropped=%llu\n",
                        g_hbTickUs / n / 1000.0, g_hbRenderUs / n / 1000.0, g_hbPresentUs / n / 1000.0,
                        (unsigned long long)g_sink->stats().fills,
                        (unsigned long long)g_sink->stats().strokes,
                        (unsigned long long)g_sink->stats().glyphs,
                        (unsigned long long)g_sink->stats().bitmaps,
                        (unsigned long long)g_sink->stats().editTexts,
                        (unsigned long long)g_sink->stats().shapesCreated,
                        (unsigned long long)g_sink->stats().masks,
                        (unsigned long long)g_sink->stats().filterLayers,
                        (unsigned long long)g_sink->stats().blurs,
                        (unsigned long long)g_sink->stats().shadows,
                        (unsigned long long)g_sink->stats().filtersDropped);
            std::fflush(stdout);
            g_hbDraws = g_hbTris = g_hbLayers = 0;
            g_hbFrames = 0;
            g_hbTickUs = g_hbRenderUs = g_hbPresentUs = 0.0;
            g_sink->resetStats();
        }

        ++frameIdx;
        ++g_tickCount;
        // 落后超过 4 帧（拖动窗口/渲染卡顿）时重新对齐，避免瞬间快放。
        if (elapsedUs - targetUs > 4LL * frameUs) {
            base = now;
            frameIdx = 0;
        }
    }
}
