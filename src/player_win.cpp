// swfplayer —— 窗口化 SWF 播放器（Win32 + GDI）。
// 行为对齐 Adobe Flash Player 独立播放器：双击/命令行传入 .swf 即开窗播放，
// 按影片帧率推进时间轴、执行 AVM1/AVM2 脚本、播放事件音/流式音，并把软件光栅器
// 的输出按舞台尺寸 1:1 呈现到窗口客户区。
//
// 入口用 main()（控制台子系统）以便错误输出；启动后立刻 FreeConsole() 只留播放窗口。
// 命令行：swfplayer <file.swf>          打开并播放
//         swfplayer <file.swf> --pause  以暂停态打开（首帧显示，等待播放）
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>
#include <string>
#include <vector>

#include "swf/Player.hpp"

namespace {

swf::Player g_player;
HWND g_hwnd = nullptr;
std::vector<std::uint8_t> g_rgba;   // 软件光栅器输出（RGBA）
std::vector<std::uint8_t> g_bgra;   // 交给 GDI 的 BGRA 副本
int g_stageW = 550, g_stageH = 400;
bool g_playing = true;
std::string g_title;

// 把当前帧渲染到指定 DC（按客户区尺寸拉伸，保持 1:1 时不失真）。
void renderToDC(HDC hdc, int clientW, int clientH) {
    int w = 0, h = 0;
    if (!g_player.render(g_rgba, w, h) || w <= 0 || h <= 0) return;
    if (g_bgra.size() != g_rgba.size()) g_bgra.resize(g_rgba.size());
    // RGBA -> BGRA（GDI 32bpp DIB 为 BGRA 顺序），并强制不透明。
    for (std::size_t i = 0; i + 3 < g_rgba.size(); i += 4) {
        g_bgra[i + 0] = g_rgba[i + 2];
        g_bgra[i + 1] = g_rgba[i + 1];
        g_bgra[i + 2] = g_rgba[i + 0];
        g_bgra[i + 3] = 0xFF;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 负值 = top-down，与 RGBA 行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, 0, 0, clientW, clientH, 0, 0, w, h,
                  g_bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// 客户区像素坐标 -> 舞台坐标（左上原点）。
void clientToStage(int cx, int cy, int clientW, int clientH, double& sx, double& sy) {
    double kx = clientW > 0 ? static_cast<double>(g_stageW) / clientW : 1.0;
    double ky = clientH > 0 ? static_cast<double>(g_stageH) / clientH : 1.0;
    sx = cx * kx;
    sy = cy * ky;
}

// 光标形状跟随悬停目标：按钮上显示手形（与 Flash Player 一致）。
void updateCursor() {
    SetCursor(LoadCursor(nullptr, g_player.handCursor() ? IDC_HAND : IDC_ARROW));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // 由 WM_PAINT 双缓冲整幅绘制，禁止擦背景防闪烁

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
            HGDIOBJ old = SelectObject(mem, bmp);
            renderToDC(mem, cw, ch);
            BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {   // 客户区内由悬停目标决定光标
                updateCursor();
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            RECT rc; GetClientRect(hwnd, &rc);
            double sx, sy;
            clientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                          rc.right - rc.left, rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            updateCursor();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            RECT rc; GetClientRect(hwnd, &rc);
            double sx, sy;
            clientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                          rc.right - rc.left, rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            g_player.mouseDown();
            return 0;
        }

        case WM_LBUTTONUP: {
            RECT rc; GetClientRect(hwnd, &rc);
            double sx, sy;
            clientToStage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                          rc.right - rc.left, rc.bottom - rc.top, sx, sy);
            g_player.setMouse(sx, sy);
            g_player.mouseUp();
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
            g_player.keyDown(static_cast<int>(wp));
            return 0;

        case WM_KEYUP:
            g_player.keyUp(static_cast<int>(wp));
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool startPaused = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pause") startPaused = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: swfplayer <file.swf> [--pause]\n");
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            path = a;
        }
    }
    if (path.empty()) {
        std::printf("usage: swfplayer <file.swf> [--pause]\n");
        return 1;
    }

    FreeConsole();   // 关掉控制台，仅留播放窗口

    std::string err;
    if (!g_player.loadFile(path.c_str(), err)) {
        std::string msg = "无法打开 " + path + "\n" + err;
        MessageBoxA(nullptr, msg.c_str(), "swfplayer", MB_ICONERROR | MB_OK);
        return 1;
    }

    // 提取文件名做标题
    std::size_t slash = path.find_last_of("\\/");
    g_title = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (g_title.empty()) g_title = path;

    g_stageW = g_player.stageW();
    g_stageH = g_player.stageH();
    g_playing = !startPaused;
    if (!g_playing) g_player.stop();

    // 舞台可能是后退到屏幕外的巨大画布（如 25600x15360 twips 的影片），
    // 若超出屏幕可用区则等比缩小窗口客户区；否则 1:1。
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int maxW = screenW - 40, maxH = screenH - 80;
    double scale = 1.0;
    if (g_stageW > maxW) scale = static_cast<double>(maxW) / g_stageW;
    if (g_stageH * scale > maxH) scale = static_cast<double>(maxH) / g_stageH;
    int clientW = static_cast<int>(g_stageW * scale + 0.5);
    int clientH = static_cast<int>(g_stageH * scale + 0.5);
    if (clientW < 1) clientW = 1;
    if (clientH < 1) clientH = 1;

    HINSTANCE hInst = GetModuleHandle(nullptr);
    const wchar_t* kClass = L"swfplayer_wnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    RECT wr{0, 0, clientW, clientH};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(
        0, kClass, L"swfplayer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) {
        MessageBoxA(nullptr, "窗口创建失败", "swfplayer", MB_ICONERROR | MB_OK);
        return 1;
    }

    std::wstring wtitle;
    wtitle.reserve(g_title.size());
    for (char c : g_title) wtitle.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    wtitle += L" - swfplayer";
    SetWindowTextW(g_hwnd, wtitle.c_str());

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    int fps = static_cast<int>(g_player.frameRate() + 0.5f);
    if (fps <= 0) fps = 12;
    // 帧调度：WM_TIMER 的精度受系统时钟限制（即使 timeBeginPeriod(1)，55 ms 的周期仍被
    // 量化到约 15.6 ms 的整数倍 → 实测 62.5 ms / 15.9 fps，整片比 SWF 标称 18 fps 慢 11%）。
    // 改用 QPC 计时 + MsgWaitForMultipleObjects 超时等待，按影片帧率精确推进。
    // timeBeginPeriod(1) 让等待超时的粒度降到 1 ms。
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
                timeEndPeriod(1);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_playing) {   // 暂停：不推进，恢复时从当前时刻重新对齐
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
        g_player.tick();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        ++frameIdx;
        // 落后超过 4 帧（拖动窗口/渲染卡顿）时重新对齐，避免连续补帧造成瞬间快放。
        if (elapsedUs - targetUs > 4LL * frameUs) {
            base = now;
            frameIdx = 0;
        }
    }
}
