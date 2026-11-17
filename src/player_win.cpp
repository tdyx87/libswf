// swfplayer —— 窗口化 SWF 播放器（Win32 + GDI）。
// 行为对齐 Adobe Flash Player 独立播放器：双击/命令行传入 .swf 即开窗播放，
// 按影片帧率推进时间轴、执行 AVM1/AVM2 脚本、播放事件音/流式音，并把软件光栅器
// 的输出按窗口客户区尺寸重新光栅化后 1:1 呈现（放大窗口不会像素化）。
//
// 入口用 main()（控制台子系统）以便错误输出；启动后立刻 FreeConsole() 只留播放窗口。
// 命令行：swfplayer <file.swf>          打开并播放
//         swfplayer <file.swf> --pause  以暂停态打开（首帧显示，等待播放）
//         swfplayer <file.swf> --record <dir> [--max-frames N] [--auto-exit]
//                                       逐帧把舞台原生尺寸渲染写入 dir\rec_NNNN.png
//                                       （程序自动录制，供 GIF 对比；录满 N 帧即停，
//                                       --auto-exit 时录完直接关窗退出）
//         swfplayer <file.swf> --click F:X:Y [--click ...]
//                                       在第 F 次 tick 向舞台 (X,Y) 注入一次左键点击
//                                       （按下 2 帧后释放，与 swfparser --click 语义一致）
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "swf/Player.hpp"
#include "stb_image_write.h"   // 实现在 swf_render(Render.cpp)；此处仅声明

namespace {

swf::Player g_player;
HWND g_hwnd = nullptr;
std::vector<std::uint8_t> g_rgba;   // 软件光栅器输出（RGBA）
std::vector<std::uint8_t> g_bgra;   // 交给 GDI 的 BGRA 副本
int g_stageW = 550, g_stageH = 400;
bool g_playing = true;
bool g_diag = false;   // --log 生效时打印窗口层诊断（坐标映射、鼠标事件）
long long g_totalTicks = 0;   // 单调 tick 计数（心跳日志用，不受掉帧归零影响）
// ---- FPS 叠加与绘制门控（静态帧跳过，见 Player::render 的 sceneFingerprint）----
// 栅格化移进主循环（rasterizeFrame），WM_PAINT 只做 1:1 贴图（blitFrame）。
// 跳过帧（指纹命中）时 g_bgra 与上一帧逐像素相同，且渲染器输出未变，
// 故 BGRA 转换与窗口重绘都可省略。以下统计量由主循环的 24-tick 心跳更新，
// WM_PAINT 只读取（同一 UI 线程，无竞争）。
volatile long long g_fps = 0;   // 心跳窗口内实际 fps（wall 口径，非目标帧率）
double g_wallMs = 0, g_cpuMs = 0, g_rastMs = 0;  // ms/帧：wall / (tick+栅格化) / 栅格化
LARGE_INTEGER g_hbT0{};
long long g_hbTicks = 0, g_hbTickUs = 0, g_hbRenderUs = 0;
bool g_needRast = true;             // 初始/窗口尺寸变化 → 强制按新视口栅格化
int g_clientW = 0, g_clientH = 0;   // 客户区尺寸（WM_SIZE/WM_PAINT 维护）
int g_bgraW = 0, g_bgraH = 0, g_bgraOx = 0, g_bgraOy = 0;   // 最近一次视口
FILE* g_log = nullptr; // 独立文件句柄：FreeConsole 后 stdout 写入不可靠，诊断走此句柄
std::string g_title;

// ---- 录制与自动点击（程序自动生成对比素材，见 docs/FIXES_LOG.md）----
void dlog(const char* fmt, ...);   // 定义见下（此块在诊断函数之前）
std::string g_recDir;                 // 非空 = 录制开启
int g_recMax = 700;                   // 最多录制帧数
int g_recCount = 0;                   // 已录制帧数
bool g_autoExit = false;              // 录满/播完自动关窗
long long g_tickCount = 0;            // tick 总数（点击调度的时基，不随掉帧对齐归零）
std::map<int, std::pair<double, double>> g_clickDown, g_clickUp;  // tick->舞台坐标
std::vector<std::uint8_t> g_recRgba;  // 录制用舞台原生尺寸缓冲

// ---- 自动保存：SWF_AUTOSAVE=cid:frame:outpath —— 当运行时某精灵(characterId=cid)
// 的播放头到达 >= frame 时，自动把当前整屏存成 outpath（一次），无需手动抓帧。
// 用于精准抓取"1042 到 f200 的拍手瞬间"这类需要交互导航才能到达的帧。
unsigned g_asCid = 0; int g_asFrame = -1; std::string g_asPath; bool g_asDone = false;
std::vector<std::uint8_t> g_asRgba;
void maybeAutoSave() {
    if (g_asDone || g_asFrame < 0 || g_asPath.empty()) return;
    int cf = g_player.runtimeSpriteFrame(g_asCid);
    if (cf < g_asFrame) return;
    int w = 0, h = 0;
    if (!g_player.render(g_asRgba, w, h, 0, 0) || w <= 0) return;
    if (stbi_write_png(g_asPath.c_str(), w, h, 4, g_asRgba.data(), w * 4))
        dlog("[autosave] cid=%u frame=%d (sprite now %d) -> %s\n", g_asCid, g_asFrame, cf, g_asPath.c_str());
    else
        dlog("[autosave] write FAILED: %s\n", g_asPath.c_str());
    dlog("%s", g_player.spriteSubtreeDump(g_asCid).c_str());   // dump 运行时子树实例位置到日志
    g_asDone = true;
    if (g_autoExit) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
}

// 把当前帧按舞台原生尺寸写入 rec_NNNN.png。返回 false 表示渲染失败或已录满。
bool recordFrame() {
    if (g_recDir.empty() || g_recCount >= g_recMax) return false;
    int w = 0, h = 0;
    if (!g_player.render(g_recRgba, w, h, 0, 0) || w <= 0 || h <= 0) return false;
    char fn[520];
    std::snprintf(fn, sizeof(fn), "%s/rec_%04d.png", g_recDir.c_str(), g_recCount);
    if (!stbi_write_png(fn, w, h, 4, g_recRgba.data(), w * 4)) {
        dlog("[rec] write FAILED: %s（目录不可写？）\n", fn);
        g_recDir.clear();   // 写失败即停录，避免每帧刷错误
        return false;
    }
    ++g_recCount;
    return true;
}

void dlog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---- M7：GDI 句柄 RAII ----
// 作用域结束时自动恢复选入对象、释放 DC/位图并 EndPaint，
// 保证 WM_PAINT 在提前返回或异常路径下也不泄漏 GDI 句柄。
// 声明顺序 paint→mem→bmp→sel，析构按逆序 sel→bmp→mem→paint，与原手工清理顺序一致。
struct ScopedPaint {
    HWND hwnd;
    PAINTSTRUCT ps{};
    HDC hdc;
    explicit ScopedPaint(HWND h) : hwnd(h), hdc(BeginPaint(h, &ps)) {}
    ~ScopedPaint() { EndPaint(hwnd, &ps); }
    ScopedPaint(const ScopedPaint&) = delete;
    ScopedPaint& operator=(const ScopedPaint&) = delete;
};
struct ScopedDC {
    HDC h = nullptr;
    explicit ScopedDC(HDC v) : h(v) {}
    ~ScopedDC() { if (h) DeleteDC(h); }
    ScopedDC(const ScopedDC&) = delete;
    ScopedDC& operator=(const ScopedDC&) = delete;
    operator HDC() const { return h; }
};
struct ScopedBitmap {
    HBITMAP h = nullptr;
    explicit ScopedBitmap(HBITMAP v) : h(v) {}
    ~ScopedBitmap() { if (h) DeleteObject(h); }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    operator HBITMAP() const { return h; }
};
struct ScopedSelect {
    HDC dc;
    HGDIOBJ old;
    ScopedSelect(HDC d, HGDIOBJ obj) : dc(d), old(obj ? SelectObject(d, obj) : nullptr) {}
    ~ScopedSelect() { if (old) SelectObject(dc, old); }
    ScopedSelect(const ScopedSelect&) = delete;
    ScopedSelect& operator=(const ScopedSelect&) = delete;
};

// Adobe Flash Player 的默认缩放模式是 Show All：舞台**等比**缩放后在客户区居中，
// 多余区域留黑边。拉伸填满客户区既会让文字位置与官方不一致，也会让圆形变椭圆。
// 这里算出实际渲染视口，渲染器按该尺寸重新光栅化，再 1:1 居中贴图。
void stageViewport(int clientW, int clientH, int& w, int& h, int& ox, int& oy) {
    if (clientW < 1) clientW = 1;
    if (clientH < 1) clientH = 1;
    const double kx = static_cast<double>(clientW) / g_stageW;
    const double ky = static_cast<double>(clientH) / g_stageH;
    const double k = kx < ky ? kx : ky;
    w = static_cast<int>(g_stageW * k + 0.5);
    h = static_cast<int>(g_stageH * k + 0.5);
    if (w < 1) w = 1; if (h < 1) h = 1;
    ox = (clientW - w) / 2;
    oy = (clientH - h) / 2;
}

// 把当前帧按视口尺寸重新光栅化（主循环调用）。
// 关键在于把 w/h 交给渲染器：矢量边缘与字形轮廓按新分辨率重新扫描填充，
// 而不是先生成舞台尺寸的小位图再用 StretchDIBits 拉伸（那样放大窗口必然像素化）。
// 静态帧跳过：Player::render 在输入集指纹不变时直接复用旧缓冲（见 Render.cpp 的
// sceneFingerprint），此时 g_rgba 与上一帧逐像素相同——g_bgra 无需重算，
// 调用方经 g_player.lastRenderRasterized() 得知本帧是否真正光栅化。
// 返回本帧光栅化耗时（微秒），供主循环累入 24-tick 心跳统计。
long long rasterizeFrame() {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int vw = 0, vh = 0, ox = 0, oy = 0;
    stageViewport(g_clientW, g_clientH, vw, vh, ox, oy);
    g_bgraW = vw; g_bgraH = vh; g_bgraOx = ox; g_bgraOy = oy;
    int w = 0, h = 0;
    if (!g_player.render(g_rgba, w, h, vw, vh) || w <= 0 || h <= 0) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        return (t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart;   // 渲染失败也返回耗时
    }
    if (g_player.lastRenderRasterized()) {
        if (g_bgra.size() != g_rgba.size()) g_bgra.resize(g_rgba.size());
        // RGBA -> BGRA（GDI 32bpp DIB 为 BGRA 顺序），并强制不透明。
        for (std::size_t i = 0; i + 3 < g_rgba.size(); i += 4) {
            g_bgra[i + 0] = g_rgba[i + 2];
            g_bgra[i + 1] = g_rgba[i + 1];
            g_bgra[i + 2] = g_rgba[i + 0];
            g_bgra[i + 3] = 0xFF;
        }
    }
    // 跳过帧：g_bgra 已是正确像素（Player::render 复用旧缓冲时 g_rgba 逐字节相同）
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart;
}

// WM_PAINT 专用：把最近一次光栅化结果 1:1 贴到 DC（不再包含光栅化本身），
// 并在左上角叠加 FPS/耗时读数。跳过的帧可以完全不调用 InvalidateRect，
// 故本函数只在真正有新像素或需要刷新叠加文字时执行。
void blitFrame(HDC hdc, int cw, int ch) {
    if (g_bgra.empty() || g_bgraW <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_bgraW;
    bi.bmiHeader.biHeight = -g_bgraH;   // 负值 = top-down，与 RGBA 行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    RECT rc{0, 0, cw, ch};
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));  // letterbox
    // 源与目标尺寸一致：1:1 贴图，不做任何插值/拉伸。
    StretchDIBits(hdc, g_bgraOx, g_bgraOy, g_bgraW, g_bgraH, 0, 0,
                  g_bgraW, g_bgraH, g_bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    // FPS 叠加（主循环心跳每 24 tick 刷新一次 g_fps/g_wallMs 等，这里只读取）：
    // 左上角黑底白字，SYSTEM_FIXED_FONT（GDI 内置，无需字体加载）。
    HFONT oldFont = (HFONT)SelectObject(hdc, GetStockObject(SYSTEM_FIXED_FONT));
    SetBkColor(hdc, RGB(0, 0, 0));
    SetTextColor(hdc, RGB(255, 255, 255));
    char overlay[128];
    std::snprintf(overlay, sizeof(overlay),
                  "FPS: %lld  wall %.1fms  tick %.1f  rast %.1f  skip %zu%%",
                  g_fps, g_wallMs, g_cpuMs - g_rastMs, g_rastMs,
                  g_player.sceneCalls() ? 100 * g_player.sceneSkips() / g_player.sceneCalls() : 0);
    RECT orct{4, 4, 460, 24};
    DrawTextA(hdc, overlay, -1, &orct, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

// 客户区像素坐标 -> 舞台坐标（左上原点）。必须走与渲染相同的 Show All 视口，
// 否则放大/留黑边后鼠标点击会整体偏移。
void clientToStage(int cx, int cy, int clientW, int clientH, double& sx, double& sy) {
    int vw = 0, vh = 0, ox = 0, oy = 0;
    stageViewport(clientW, clientH, vw, vh, ox, oy);
    sx = (cx - ox) * (static_cast<double>(g_stageW) / vw);
    sy = (cy - oy) * (static_cast<double>(g_stageH) / vh);
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
            ScopedPaint paint(hwnd);
            if (!paint.hdc) return 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            g_clientW = cw > 0 ? cw : 1;
            g_clientH = ch > 0 ? ch : 1;
            ScopedDC mem(CreateCompatibleDC(paint.hdc));
            ScopedBitmap bmp(CreateCompatibleBitmap(paint.hdc, cw > 0 ? cw : 1, ch > 0 ? ch : 1));
            if (!mem.h || !bmp.h) return 0;
            ScopedSelect sel(mem, bmp.h);
            blitFrame(mem, cw, ch);   // 仅 1:1 贴图 + FPS 叠加；光栅化在主循环做
            BitBlt(paint.hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            return 0;   // 析构逆序自动完成 SelectObject/DeleteObject/DeleteDC/EndPaint
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_clientW = LOWORD(lp) > 0 ? LOWORD(lp) : 1;
                g_clientH = HIWORD(lp) > 0 ? HIWORD(lp) : 1;
                g_needRast = true;    // 视口变化 → 按新尺寸重新光栅化
            }
            return 0;

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
            if (g_diag)
                dlog("[win] LBUTTONDOWN client=(%d,%d) clientWH=%dx%d stage=(%.1f,%.1f)\n",
                     GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
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
            if (g_diag)
                dlog("[win] LBUTTONUP stage=(%.1f,%.1f)\n", sx, sy);
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
    // 高 DPI：不声明 DPI 感知时 Windows 会把整个窗口做位图虚拟化拉伸，
    // 即使光栅器已按客户区重绘，最终显示仍会被系统再放大一次而发虚。
    // 用 GetProcAddress 以兼容没有该 API 的旧系统。
    if (HMODULE u32 = LoadLibraryW(L"user32.dll")) {
        using SetDPIAwareFn = BOOL (WINAPI *)();
        auto fn = reinterpret_cast<SetDPIAwareFn>(GetProcAddress(u32, "SetProcessDPIAware"));
        if (fn) fn();
        FreeLibrary(u32);
    }
    // 逐节点自身内容栅格缓存：默认关闭（SWF_RASTER_CACHE 未设时）。
    // 修复 enabled() 指针门控（commit 见 swfparser flashplaer）后的实测（Baby_Care_Spa_Salon
    // 菜单段 600 帧，6 次点击，缓存开关真正生效）：
    //   OFF = 63.6ms/帧（与无缓存版本 41b11ba 的 63.9ms/帧 持平）
    //   ON  = 99.7ms/帧（命中率 77%，但 512 条目 cap 下 LRU 抖动 evict=31949，
    //         且每次命中仍要按紧致包围盒做一遍 over 合成，净开销大于直接光栅化）
    // 即当前缓存实现在该场景下是负优化，默认保持关闭；外部 SWF_RASTER_CACHE=1 可覆盖。
    // 若日后把缓存改成"仅内容键 + 局部空间"（动画节点不再每帧 miss）再重新评估默认值。
    if (!std::getenv("SWF_RASTER_CACHE")) _putenv_s("SWF_RASTER_CACHE", "0");
    std::string path;
    std::string logPath;
    bool startPaused = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pause") startPaused = true;
        else if (a == "--log" && i + 1 < argc) logPath = argv[++i];   // 诊断日志输出到文件
        else if (a == "--record" && i + 1 < argc) g_recDir = argv[++i];
        else if (a == "--max-frames" && i + 1 < argc) g_recMax = std::atoi(argv[++i]);
        else if (a == "--auto-exit") g_autoExit = true;
        else if (a == "--save-at" && i + 1 < argc) {   // --save-at <cid>:<frame>:<outpath>
            const std::string spec = argv[++i];
            char p[512] = {0};
            if (std::sscanf(spec.c_str(), "%u:%d:%511s", &g_asCid, &g_asFrame, p) == 3) {
                g_asPath = p;
                std::printf("[save-at] will capture when runtime sprite cid=%u reaches frame %d -> %s\n",
                            g_asCid, g_asFrame, g_asPath.c_str());
            }
        }
        else if (a == "--click" && i + 1 < argc) {   // --click <tick>:<x>:<y>
            const std::string spec = argv[++i];
            int fr = 0; double x = 0, y = 0;
            if (std::sscanf(spec.c_str(), "%d:%lf:%lf", &fr, &x, &y) == 3) {
                g_clickDown[fr] = {x, y};
                g_clickUp[fr + 2] = {x, y};          // 按住 2 帧后释放
            }
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: swfplayer <file.swf> [--pause] [--log <file>] "
                        "[--record <dir>] [--max-frames N] [--auto-exit] "
                        "[--click F:X:Y]...\n");
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            path = a;
        }
    }
    if (path.empty()) {
        std::printf("usage: swfplayer <file.swf> [--pause] [--log <file>]\n");
        return 1;
    }

    // --log：诊断写入独立文件句柄；同时把 stdout 与 stderr 都重定向过去。
    // 引擎内部日志（SWF_AS_DEBUG 的 [mouse]/[tree]/[as]、SWF_TWEEN_DBG 的 [tween]、
    // SWF_AS_MISS 的 [miss]）一律写 stderr，此前只重定向 stdout，GUI 子系统下
    // stderr 无处可去，这些开关在 swfplayer 里等于完全无输出。
    if (!logPath.empty()) {
        if (FILE* t = fopen(logPath.c_str(), "w")) fclose(t);   // 先清空旧日志
        g_log = fopen(logPath.c_str(), "a");                    // 追加模式：与 stdout 共写不互相覆盖
        if (g_log) {
            g_diag = true;
            freopen(logPath.c_str(), "a", stdout);
            setvbuf(stdout, nullptr, _IONBF, 0);
            freopen(logPath.c_str(), "a", stderr);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }
    dlog("[win] === swfplayer start === file=%s\n", path.c_str());
    // --log 时保留控制台：FreeConsole 后 CRT stdout 写入会静默失败，
    // 引擎内部日志（SWF_AS_DEBUG 的 [mouse]/[tree]）就全丢了。
    if (logPath.empty()) FreeConsole();
    dlog("[win] console state ok, loading...\n");

    std::string err;
    if (!g_player.loadFile(path.c_str(), err)) {
        dlog("[win] loadFile FAILED: %s\n", err.c_str());
        std::string msg = "无法打开 " + path + "\n" + err;
        MessageBoxA(nullptr, msg.c_str(), "swfplayer", MB_ICONERROR | MB_OK);
        return 1;
    }
    dlog("[win] loaded stage=%dx%d frames=%d fps=%.1f vms=%zu runtimeRoots=%zu\n",
         g_player.stageW(), g_player.stageH(), g_player.totalFrames(),
         g_player.frameRate(), g_player.vmCount(), g_player.runtimeRoots());

    // 录制目录提前创建（失败则关闭录制并提示）
    if (!g_recDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_recDir, ec);
        if (ec) {
            dlog("[rec] create dir FAILED: %s (%s)\n", g_recDir.c_str(), ec.message().c_str());
            g_recDir.clear();
        }
    }

    // 提取文件名做标题
    std::size_t slash = path.find_last_of("\\/");
    g_title = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (g_title.empty()) g_title = path;

    g_stageW = g_player.stageW();
    g_stageH = g_player.stageH();
    g_playing = !startPaused;
    if (!g_playing) {
        // 暂停态同样要先让首帧显示列表/帧脚本就位：load() 之后直接 render()
        // 时 curItems_ 还是空的，窗口会是一片黑（放大窗口时尤其明显）。
        g_player.tick();
        g_player.stop();
    }

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
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        g_player.tick();
        // 自动点击调度（与渲染/时间轴同一时基）：按下在 tick F，释放在 tick F+2
        {
            auto dn = g_clickDown.find(static_cast<int>(g_tickCount));
            if (dn != g_clickDown.end()) {
                g_player.setMouse(dn->second.first, dn->second.second);
                g_player.mouseDown();
            }
            auto up = g_clickUp.find(static_cast<int>(g_tickCount));
            if (up != g_clickUp.end()) {
                g_player.setMouse(up->second.first, up->second.second);
                g_player.mouseUp();
            }
        }
        // 光栅化（静态帧跳过时 Player::render 直接返回旧缓冲，无整帧光栅化开销）
        const long long rastUs = rasterizeFrame();
        // 必须在 recordFrame/maybeAutoSave **之前**读取：两者以舞台原生尺寸 (0,0)
        // 渲染，会覆盖 lastRenderRasterized。
        const bool frameRasterized = g_player.lastRenderRasterized();
        // 录制：每 tick 存一帧舞台原生尺寸 PNG
        if (!g_recDir.empty() && recordFrame()) {
            if (g_recCount == 1) dlog("[rec] start -> %s\n", g_recDir.c_str());
            if (g_recCount >= g_recMax) {
                dlog("[rec] done: %d frames\n", g_recCount);
                if (g_autoExit) { PostMessage(g_hwnd, WM_CLOSE, 0, 0); }
            }
        }
        maybeAutoSave();   // 运行时精灵到达目标帧则自动存图
        QueryPerformanceCounter(&t1);
        const long long cpuUs = (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
        ++frameIdx;
        ++g_tickCount;
        // 24-tick 心跳：刷新 FPS 叠加的统计量（wall = 真实吞吐；cpu = tick+光栅化）。
        // 用单调计数（frameIdx 掉帧超 4 帧会归零，慢速渲染下永远到不了周期数）。
        ++g_hbTicks;
        {
            LARGE_INTEGER hnow;
            QueryPerformanceCounter(&hnow);
            if (g_hbTicks == 1) g_hbT0 = hnow;
            if (g_hbTicks % 24 == 0) {
                const long long hbUs = (hnow.QuadPart - g_hbT0.QuadPart) * 1000000 / freq.QuadPart;
                g_fps = (hbUs > 0) ? 24LL * 1000000LL / hbUs : 0;
                g_wallMs = static_cast<double>(hbUs) / 24.0 / 1000.0;
                g_cpuMs = static_cast<double>(g_hbTickUs) / 24.0 / 1000.0;
                g_rastMs = static_cast<double>(g_hbRenderUs) / 24.0 / 1000.0;
                g_hbTickUs = 0;
                g_hbRenderUs = 0;
                g_hbT0 = hnow;
                if (g_diag && ++g_totalTicks % 48 == 0)
                    dlog("[win] totalTicks=%lld frameIdx=%lld mainFrame=%d wall=%.2fms cpu=%.2fms rast=%.2fms fps=%lld\n",
                         g_totalTicks, frameIdx, g_player.currentFrame(), g_wallMs, g_cpuMs,
                         g_rastMs, g_fps);
            }
        }
        g_hbTickUs += cpuUs;
        g_hbRenderUs += rastUs;
        // 绘制门控：跳过帧的像素与上一帧逐像素相同（Player::render 复用旧缓冲），
        // 不必重绘；FPS 叠加每 24 tick 刷新一次统计量，窗口尺寸变化（g_needRast）
        // 与暂停态恢复也必须重绘。代价上限：最坏每 24 tick 多一次 ~1ms 的 1:1 贴图。
        if (!g_playing || g_needRast || frameRasterized || (g_hbTicks % 24 == 0))
            InvalidateRect(g_hwnd, nullptr, FALSE);
        g_needRast = false;
        // 落后超过 4 帧（拖动窗口/渲染卡顿）时重新对齐，避免连续补帧造成瞬间快放。
        if (elapsedUs - targetUs > 4LL * frameUs) {
            base = now;
            frameIdx = 0;
        }
    }
}
