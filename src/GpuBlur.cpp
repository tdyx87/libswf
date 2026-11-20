// 方案 B：GPU 可分离高斯模糊（D3D11 计算着色器）。
//
// 架构（2026-11 独立冒烟 d3d11_diag2..7 逐条验证后定稿）：
//  - 本环境 D3D11 光栅化器（Draw）静默失效（虚拟化 GPU），计算管线（Dispatch）
//    完全可用 → H/V 两趟改用计算着色器，绕开光栅化路径；
//  - 单调缓冲流：b0(SRV) → b1(SRV+UAV) → b2(UAV)。缓冲一旦被 UAV 写过就不再
//    作 SRV（该虚拟 GPU 驱动缺陷：SRV→UAV 复用的目标缓冲输出恒零，diag6 证实）；
//    UAV→SRV 方向复用安全（diag7 三次 ping-pong 全对）。
//  - 两趟之间完全解绑（SRV 槽 0..1 与 UAV 槽 0 全部置 null）触发隐式状态转换
//    —— 该虚拟 GPU 缺少 D3D11 隐式 transition 且本机 SDK 头无 ResourceBarrier
//    定义；部分解绑（只清 UAV）会访问违例（diag6），完全解绑可靠（diag7）。
//  - 逐位一致：累加顺序 -R..+R 与 CPU 相同；float4 承载 0..255 精确整数；
//    truncByte = clamp+floor ≡ CPU 的 clampU8((int)(acc+0.5))；
//    独立冒烟 4/4 用例 119032 通道 0 差异 maxDelta=0（diag5）。
//  - 512x512 R=24 完整往返（上传+H+V+读回）GPU 15.7ms vs CPU 67.0ms（4.3x，diag5）。
//  - 降/升采样留 CPU（算术不变）；任何失败 → false → 调用方走原 CPU 路径。
//
// 单线程使用（渲染线程）；COM 对象不跨线程。
#include "swf/GpuBlur.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace swf {
namespace gpublur {
namespace {

constexpr int kKernelCap = 512;                 // 核元素上限（最大 r=24 → 49）
constexpr int kWinCacheCap = 8;                 // 窗口尺寸缓存上限
constexpr std::size_t kMaxPx = 16u * 1024u * 1024u;  // 单窗上限

// 一维 H 卷积（边缘 clamp）。P = {W, H, R, 0}，Kern = [k_{-R}..k_{+R}]。
// 与 CPU 完全同序：acc += Src(s)*Kern(i+R)，i=-R..+R；truncByte ≡ CPU 舍入。
const char* kConvH =
    "RWStructuredBuffer<float4> Dst : register(u0);\n"
    "StructuredBuffer<float4> Src : register(t0);\n"
    "StructuredBuffer<float> Kern : register(t1);\n"
    "cbuffer Cb : register(b0) { float4 P; }\n"
    "float truncByte(float v)\n"
    "    { v = v > 255.0 ? 255.0 : (v < 0.0 ? 0.0 : v); return floor(v); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID)\n"
    "{\n"
    "    int W = (int)P.x, H = (int)P.y, R = (int)P.z;\n"
    "    int x = (int)tid.x, y = (int)tid.y;\n"
    "    if (x >= W || y >= H) return;\n"
    "    float4 acc = 0.0;\n"
    "    for (int i = -R; i <= R; i++)\n"
    "    {\n"
    "        int s = x + i;\n"
    "        if (s < 0) s = 0;\n"
    "        else if (s >= W) s = W - 1;\n"
    "        acc += Src.Load(s + y * W) * Kern.Load(i + R);\n"
    "    }\n"
    "    float4 v = acc + 0.5;\n"
    "    Dst[y * W + x] = float4(truncByte(v.x), truncByte(v.y), truncByte(v.z), truncByte(v.w));\n"
    "}\n";

const char* kConvV =
    "RWStructuredBuffer<float4> Dst : register(u0);\n"
    "StructuredBuffer<float4> Src : register(t0);\n"
    "StructuredBuffer<float> Kern : register(t1);\n"
    "cbuffer Cb : register(b0) { float4 P; }\n"
    "float truncByte(float v)\n"
    "    { v = v > 255.0 ? 255.0 : (v < 0.0 ? 0.0 : v); return floor(v); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID)\n"
    "{\n"
    "    int W = (int)P.x, H = (int)P.y, R = (int)P.z;\n"
    "    int x = (int)tid.x, y = (int)tid.y;\n"
    "    if (x >= W || y >= H) return;\n"
    "    float4 acc = 0.0;\n"
    "    for (int i = -R; i <= R; i++)\n"
    "    {\n"
    "        int s = y + i;\n"
    "        if (s < 0) s = 0;\n"
    "        else if (s >= H) s = H - 1;\n"
    "        acc += Src.Load(s * W + x) * Kern.Load(i + R);\n"
    "    }\n"
    "    float4 v = acc + 0.5;\n"
    "    Dst[y * W + x] = float4(truncByte(v.x), truncByte(v.y), truncByte(v.z), truncByte(v.w));\n"
    "}\n";

struct Ctx {
    bool inited = false;
    bool ok = false;
    int tries = 0;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* dc = nullptr;
    ID3D11ComputeShader* csH = nullptr;
    ID3D11ComputeShader* csV = nullptr;
    ID3D11Buffer* kern = nullptr;              // 512 float（动态）
    ID3D11ShaderResourceView* kernSrv = nullptr;
    ID3D11Buffer* cb = nullptr;                // 16B cbuffer（动态，单次 UpdateSubresource）
    // 共享上传缓存：kern/cb 是全上下文单缓冲，缓存状态必须随之全局，
    // 否则窗口 A 缓存的"最新值"可能被窗口 B 覆盖而误判跳过上传
    int cbW = 0, cbH = 0, cbR = 0;
    unsigned kernSig = 0xEu;                   // 0xEu = 从未上传
};
Ctx& ctx() { static Ctx c; return c; }

// 单调缓冲流：b0 只作 SRV，b1 = SRV+UAV，b2 只作 UAV（另含上传/读回 staging）。
struct Win {
    int W = 0, H = 0;
    ID3D11Buffer* b0 = nullptr;
    ID3D11Buffer* b1 = nullptr;
    ID3D11Buffer* b2 = nullptr;
    ID3D11ShaderResourceView* s0 = nullptr;
    ID3D11ShaderResourceView* s1 = nullptr;
    ID3D11UnorderedAccessView* u1 = nullptr;
    ID3D11UnorderedAccessView* u2 = nullptr;
    ID3D11Buffer* sw = nullptr;   // 上传 staging（CPU_WRITE）
    ID3D11Buffer* sr = nullptr;   // 读回 staging（CPU_READ）
};
std::vector<Win> g_wins;
static long g_winsCreated = 0, g_winsEvicted = 0;   // churn 检测（getWin 用）

void winRelease(Win& w) {
    if (w.s0) w.s0->Release();
    if (w.s1) w.s1->Release();
    if (w.u1) w.u1->Release();
    if (w.u2) w.u2->Release();
    if (w.b0) w.b0->Release();
    if (w.b1) w.b1->Release();
    if (w.b2) w.b2->Release();
    if (w.sw) w.sw->Release();
    if (w.sr) w.sr->Release();
    w = Win{};
}

bool makeWin(ID3D11Device* dev, int W, int H, Win& w) {
    w.W = W;
    w.H = H;
    const UINT byteW = static_cast<UINT>(static_cast<std::size_t>(W) * H * 16);
    const UINT nElem = static_cast<UINT>(static_cast<std::size_t>(W) * H);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = byteW;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &w.b0)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &w.b1)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &w.b2))) {
        winRelease(w);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = nElem;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.FirstElement = 0;
    uv.Buffer.NumElements = nElem;
    if (FAILED(dev->CreateShaderResourceView(w.b0, &sv, &w.s0)) ||
        FAILED(dev->CreateShaderResourceView(w.b1, &sv, &w.s1)) ||
        FAILED(dev->CreateUnorderedAccessView(w.b1, &uv, &w.u1)) ||
        FAILED(dev->CreateUnorderedAccessView(w.b2, &uv, &w.u2))) {
        winRelease(w);
        return false;
    }
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = byteW;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &w.sw))) { winRelease(w); return false; }
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &w.sr))) { winRelease(w); return false; }
    return true;
}

Win* getWin(int W, int H) {
    for (std::size_t i = 0; i < g_wins.size(); ++i) {
        if (g_wins[i].W == W && g_wins[i].H == H) {
            if (i + 1 < g_wins.size()) {   // 简易 LRU：移到尾部
                Win t = std::move(g_wins[i]);
                g_wins.erase(g_wins.begin() + static_cast<std::ptrdiff_t>(i));
                g_wins.push_back(std::move(t));
            }
            return &g_wins.back();
        }
    }
    if (g_wins.size() >= kWinCacheCap) {
        winRelease(g_wins.front());
        g_wins.erase(g_wins.begin());
        ++g_winsEvicted;
    }
    Win w;
    if (!makeWin(ctx().dev, W, H, w)) return nullptr;
    ++g_winsCreated;
    g_wins.push_back(std::move(w));
    return &g_wins.back();
}

// 核的 FNV-1a 签名：签名不变则跳过重传（≤49 float，O(1) 级开销）
static unsigned kernSig(const std::vector<float>& k) {
    std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(k.size());
    for (float x : k) {
        std::uint32_t bits;
        std::memcpy(&bits, &x, 4);
        h ^= bits;
        h *= 16777619u;
    }
    return h;
}

bool uploadKern(Ctx& c, const std::vector<float>& k) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c.dc->Map(c.kern, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    std::memset(m.pData, 0, static_cast<std::size_t>(kKernelCap) * 4);
    std::memcpy(m.pData, k.data(), k.size() * 4);
    c.dc->Unmap(c.kern, 0);
    return true;
}

bool setCb(Ctx& c, int W, int H, int r) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c.dc->Map(c.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    const float data[4] = {static_cast<float>(W), static_cast<float>(H),
                           static_cast<float>(r), 0.0f};
    std::memcpy(m.pData, data, 16);
    c.dc->Unmap(c.cb, 0);
    return true;
}

// 完全解绑：SRV 槽 0..1 与 UAV 槽 0 置 null。
// 该虚拟 GPU 无 D3D11 隐式状态转换（且本机 SDK 头缺 ResourceBarrier 定义），
// 完全解绑-重绑是验证过的隐式 transition 触发方式（diag7）；部分解绑会 AV。
void unbindAll(Ctx& c) {
    ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* nullu[1] = {nullptr};
    c.dc->CSSetShaderResources(0, 2, nulls);
    c.dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr);
}

// [STAT] SWF_GPUBLUR_STAT=1：进程退出时打印逐调用统计（定位开销分布）
struct Stat {
    long calls = 0;      // 进入 blurRgba 且通过前置检查
    long ok = 0;         // GPU 成功
    unsigned long long px = 0;   // 成功处理的像素
    double ms = 0.0;     // 成功调用的墙钟合计
    double minPx_ = 1e18, maxPx_ = 0;
    // 尺寸直方图（定位开销分布）：bin = [1k,4k),[4k,16k),[16k,64k),[64k,256k),[256k,)
    long binCalls[5] = {0,0,0,0,0};
    double binMs[5] = {0,0,0,0,0};
    unsigned long long binPx[5] = {0,0,0,0,0};
};
static Stat g_stat;
static void statAtExit() {
    if (!std::getenv("SWF_GPUBLUR_STAT")) return;
    const char* bn[5] = {"1-4k", "4-16k", "16-64k", "64-256k", "256k+"};
    std::fprintf(stderr,
                 "[gpublur][stat] calls=%ld ok=%llu px=%.0f ms=%.1f "
                 "avg_px=%.0f avg_ms=%.4f winPx=%.0f..%.0f wins(created=%ld evicted=%ld)\n",
                 g_stat.calls, (unsigned long long)g_stat.ok,
                 (double)g_stat.px, g_stat.ms,
                 g_stat.ok ? (double)g_stat.px / g_stat.ok : 0.0,
                 g_stat.ok ? g_stat.ms / g_stat.ok : 0.0,
                 g_stat.minPx_ == 1e18 ? 0 : g_stat.minPx_,
                 g_stat.maxPx_, g_winsCreated, g_winsEvicted);
    for (int b = 0; b < 5; ++b)
        std::fprintf(stderr,
                     "[gpublur][stat]   bin %6s: calls=%6ld px=%10llu ms=%9.2f avg_ms=%.4f\n",
                     bn[b], g_stat.binCalls[b], (unsigned long long)g_stat.binPx[b],
                     g_stat.binMs[b],
                     g_stat.binCalls[b] ? g_stat.binMs[b] / g_stat.binCalls[b] : 0.0);
}

// [DIAG] SWF_GPUBLUR_DIAG=1：读回缓冲并打印 alpha 质量/首个非零像素。
static void diagLog(const char* tag, Ctx& c, Win& w, int W, int H,
                    ID3D11Buffer* b) {
    if (!std::getenv("SWF_GPUBLUR_DIAG")) return;
    c.dc->CopyResource(w.sr, b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c.dc->Map(w.sr, 0, D3D11_MAP_READ, 0, &m))) return;
    const float* f = static_cast<const float*>(m.pData);
    long sum = 0;
    int fx = -1, fy = -1;
    const std::size_t n = static_cast<std::size_t>(W) * H;
    for (std::size_t p = 0; p < n; ++p) {
        const float a = f[p * 4 + 3];
        if (a > 0.5f) {
            sum += static_cast<int>(a + 0.5f);
            if (fx < 0) { fx = static_cast<int>(p % W); fy = static_cast<int>(p / W); }
        }
    }
    std::fprintf(stderr, "[gpublur][diag] %s alphaMass=%ld firstInk=(%d,%d)\n",
                 tag, sum, fx, fy);
    c.dc->Unmap(w.sr, 0);
}

bool envOn(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return true;   // 默认 ON
    return std::strncmp(v, "0", 1) != 0 && std::strcmp(v, "off") != 0 &&
           std::strcmp(v, "no") != 0;
}

std::size_t minPx() {
    static const std::size_t v = [] {
        const char* s = std::getenv("SWF_GPU_BLUR_MIN_PX");
        std::size_t n = 1024;
        if (s && *s) n = static_cast<std::size_t>(std::strtoul(s, nullptr, 10));
        return n;
    }();
    return v;
}

ID3DBlob* compileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry,
                            target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &err);
    if (FAILED(hr) || !code) {
        if (err) {
            std::fprintf(stderr, "[gpublur] shader compile failed: %s\n",
                         static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

void cleanup(Ctx& c) {
    for (auto& w : g_wins) winRelease(w);
    g_wins.clear();
    if (c.kernSrv) c.kernSrv->Release();
    if (c.kern) c.kern->Release();
    if (c.cb) c.cb->Release();
    if (c.csV) c.csV->Release();
    if (c.csH) c.csH->Release();
    if (c.dc) c.dc->Release();
    if (c.dev) c.dev->Release();
    c = Ctx{};
    c.inited = true;
}

// —— 本地 CPU 分离高斯 H/V 参照（与 Render.cpp blurBufferCore d==1 同算术：
//    逐通道 float 累加、每趟后字节截断 clampU8((int)(acc+0.5f))）。
//    仅供自动探测的计时对照，保证 GPU 往返与 CPU 卷积成本可比。——
static std::uint8_t pClampU8(int v) { return (v < 0) ? 0 : (v > 255) ? 255 : static_cast<std::uint8_t>(v); }
static void probeCpuHv(std::uint8_t* buf, int W, int H,
                       const std::vector<float>& kx, const std::vector<float>& ky) {
    const int rx = static_cast<int>(kx.size()) / 2;
    const int ry = static_cast<int>(ky.size()) / 2;
    static thread_local std::vector<std::uint8_t> tmp;
    tmp.assign(static_cast<std::size_t>(W) * H * 4, 0);
    std::uint8_t* t = tmp.data();
    if (rx >= 1) {
        for (int y = 0; y < H; ++y) {
            const std::uint8_t* row = &buf[static_cast<std::size_t>(y) * W * 4];
            std::uint8_t* o = &t[static_cast<std::size_t>(y) * W * 4];
            for (int x = 0; x < W; ++x) {
                float acc[4] = {0,0,0,0};
                for (int i = -rx; i <= rx; ++i) {
                    const int xx = std::min(W-1, std::max(0, x+i));
                    const float kv = kx[static_cast<std::size_t>(i+rx)];
                    const std::uint8_t* s = &row[xx*4];
                    for (int cc = 0; cc < 4; ++cc) acc[cc] += s[cc] * kv;
                }
                for (int cc = 0; cc < 4; ++cc) o[x*4+cc] = pClampU8(static_cast<int>(acc[cc] + 0.5f));
            }
        }
    } else std::memcpy(t, buf, static_cast<std::size_t>(W)*H*4);
    if (ry >= 1) {
        for (int y = 0; y < H; ++y) {
            std::uint8_t* o = &buf[static_cast<std::size_t>(y) * W * 4];
            for (int x = 0; x < W; ++x) {
                float acc[4] = {0,0,0,0};
                for (int j = -ry; j <= ry; ++j) {
                    const int yy = std::min(H-1, std::max(0, y+j));
                    const float kv = ky[static_cast<std::size_t>(j+ry)];
                    const std::uint8_t* s = &t[(static_cast<std::size_t>(yy)*W+x)*4];
                    for (int cc = 0; cc < 4; ++cc) acc[cc] += s[cc] * kv;
                }
                for (int cc = 0; cc < 4; ++cc) o[x*4+cc] = pClampU8(static_cast<int>(acc[cc] + 0.5f));
            }
        }
    } else std::memcpy(buf, t, static_cast<std::size_t>(W)*H*4);
}

// 自动探测：按实测生产调用分布（1-4k ~92% / 4-16k ~7% / 16-64k ~1%）
// 加权跑 GPU 完整往返 vs CPU H/V，GPU 总耗时须比 CPU 快 >=5% 才启用。
// 返回 true 表示应启用 GPU。SWF_GPU_BLUR_FORCE=1 由调用方跳过本函数。
static bool probeGpuFaster(Ctx& c) {
    // 核：r=6（与实测平均半径 ~5 同量级），H/V 对称。
    const int r = 6;
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    const double inv2s2 = 1.0 / (2.0 * 4.0 * 4.0);   // σ=4 → r=ceil(3*4)=12 过大；改用 σ 使 r=6：σ=2
    double sum = 0.0;
    for (int i = -r; i <= r; ++i) {
        const double v = std::exp(-static_cast<double>(i) * i * inv2s2);
        k[static_cast<std::size_t>(i + r)] = static_cast<float>(v);
        sum += v;
    }
    const float inv = static_cast<float>(1.0 / sum);
    for (auto& x : k) x *= inv;

    struct Case { int W, H; double w; };
    // 权重 = 生产调用频次（实测 1600 步：1-4k ~92% / 4-16k ~7% / 16-64k ~1%）。
    // 必须按频次加权，否则中/大窗的算力优势会掩盖占 92% 的小窗固定开销劣势。
    const Case cases[] = {
        {48, 44, 0.92},    // ~2.1k px，对齐 bin 1-4k 主导
        {96, 64, 0.07},    // ~6.1k px，对齐 bin 4-16k
        {160, 128, 0.01},  // ~20.5k px，对齐 bin 16-64k
    };
    const long N = 120;                       // 每类样本数
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(160) * 128 * 4, 0);
    unsigned seed = 0xC0FFEEu;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<unsigned>(seed >> 16) & 255u; };
    for (auto& b : buf) b = static_cast<std::uint8_t>(rnd());

    LARGE_INTEGER qf, t0, t1;
    QueryPerformanceFrequency(&qf);
    double gpuW = 0.0, cpuW = 0.0;
    for (const auto& cs : cases) {
        const int W = cs.W, H = cs.H;
        // GPU 往返
        QueryPerformanceCounter(&t0);
        for (long i = 0; i < N; ++i)
            if (!blurRgba(buf.data(), W, H, k, k)) { std::fprintf(stderr, "[gpublur] probe: GPU roundtrip failed -> disabled\n"); return false; }
        QueryPerformanceCounter(&t1);
        const double gpuOne = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart / N;
        // CPU 参照（同分布）
        QueryPerformanceCounter(&t0);
        for (long i = 0; i < N; ++i)
            probeCpuHv(buf.data(), W, H, k, k);
        QueryPerformanceCounter(&t1);
        const double cpuOne = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart / N;
        std::fprintf(stderr, "[gpublur] probe  %dx%d: gpu=%.3fms cpu=%.3fms\n",
                     W, H, gpuOne, cpuOne);
        gpuW += cs.w * gpuOne;
        cpuW += cs.w * cpuOne;
    }
    const bool faster = gpuW < cpuW * 0.95;
    std::fprintf(stderr, "[gpublur] probe: weighted gpu=%.3fms cpu=%.3fms ratio=%.2f -> %s\n",
                 gpuW, cpuW, cpuW / gpuW, faster ? "GPU enabled" : "CPU (GPU disabled)");
    return faster;
}

}  // namespace

bool init() {
    Ctx& c = ctx();
    if (c.inited) return c.ok;
    c.inited = true;
    if (!envOn("SWF_GPU_BLUR")) return false;
    if (c.tries >= 2) return false;
    ++c.tries;
    std::atexit(statAtExit);

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &c.dev, &fl, &c.dc);
    if (FAILED(hr)) {   // 无硬件 GPU（虚拟机等）→ WARP 兜底（计算管线亦可运行）
        c.dev = nullptr;
        c.dc = nullptr;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_SOFTWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &c.dev, &fl, &c.dc);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "[gpublur] D3D11 device create failed (hr=0x%08X)\n",
                     static_cast<unsigned>(hr));
        return false;
    }

    ID3DBlob* code = compileShader(kConvH, "main", "cs_5_0");
    if (!code) { cleanup(c); return false; }
    if (FAILED(c.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                          nullptr, &c.csH))) {
        code->Release();
        cleanup(c);
        return false;
    }
    code->Release();
    code = compileShader(kConvV, "main", "cs_5_0");
    if (!code) { cleanup(c); return false; }
    if (FAILED(c.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                          nullptr, &c.csV))) {
        code->Release();
        cleanup(c);
        return false;
    }
    code->Release();

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = static_cast<UINT>(kKernelCap * 4);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(c.dev->CreateBuffer(&bd, nullptr, &c.kern))) { cleanup(c); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R32_FLOAT;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    s.Buffer.FirstElement = 0;
    s.Buffer.NumElements = kKernelCap;
    if (FAILED(c.dev->CreateShaderResourceView(c.kern, &s, &c.kernSrv))) { cleanup(c); return false; }

    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(c.dev->CreateBuffer(&bd, nullptr, &c.cb))) { cleanup(c); return false; }

    c.ok = true;
    // 自动探测：本环境（虚拟化 GPU）下 GPU 每调用固定开销 ~0.3ms，对生产分布
    // （小窗、短核）常慢于 CPU。探测一次生产分布加权微基准，GPU 须快 >=5% 才启用，
    // 否则回退 CPU（像素逐位一致，仅性能选择）。SWF_GPU_BLUR_FORCE=1 强制启用、跳过探测。
    const char* force = std::getenv("SWF_GPU_BLUR_FORCE");
    if (!force || force[0] == '\0' || std::strcmp(force, "0") == 0 ||
        std::strcmp(force, "off") == 0 || std::strcmp(force, "no") == 0) {
        if (!probeGpuFaster(c)) {
            c.ok = false;   // GPU 未胜出 → 保持 CPU 回退；设备资源保留（inited=true 不再重建）
        }
    } else {
        std::fprintf(stderr, "[gpublur] probe: forced (SWF_GPU_BLUR_FORCE) -> GPU enabled\n");
    }
    g_stat = Stat{};   // 清零探测期统计，最终直方图只反映生产调用
    std::fprintf(stderr, "[gpublur] ready v5-compute (fl=0x%04X, %s)\n",
                 static_cast<unsigned>(fl), c.ok ? "gpu" : "cpu-fallback");
    return c.ok;
}

// 核心：RGBA 预乘缓冲就地 H/V 两趟（单调流 b0→b1→b2 + 完全解绑）。
bool blurRgba(std::uint8_t* buf, int W, int H,
              const std::vector<float>& kx, const std::vector<float>& ky) {
    Ctx& c = ctx();
    if (!c.ok) { if (!init()) return false; }
    if (W <= 0 || H <= 0) return false;
    if (static_cast<std::size_t>(W) * H < minPx()) return false;
    if (static_cast<std::size_t>(W) * H > kMaxPx) return false;
    if (kx.size() > kKernelCap || ky.size() > kKernelCap) return false;
    const int rx = static_cast<int>(kx.size()) / 2;
    const int ry = static_cast<int>(ky.size()) / 2;
    if (rx < 1 && ry < 1) return false;   // 两方向皆恒等，无需 GPU
    Win* w = getWin(W, H);
    if (!w) return false;   // 资源创建失败（如显存不足）→ 本次回退 CPU（LRU 有界，可重试）
    ++g_stat.calls;
    const std::size_t pxNow = static_cast<std::size_t>(W) * H;
    LARGE_INTEGER qfq, q0, q1;
    QueryPerformanceFrequency(&qfq);
    QueryPerformanceCounter(&q0);

    // 输入（uint8 RGBA）→ 上传 staging → b0（float4，0..255 精确）。
    // 用 staging CopyResource + Map(WRITE_DISCARD)：本虚拟 GPU 上 UpdateSubresource
    // 静默失效（b0 读回为垃圾值），staging 路径实测可靠（diag5 全 0 差异）。
    static thread_local std::vector<float> up;
    up.resize(static_cast<std::size_t>(W) * H * 4);
    for (std::size_t p = 0, n = up.size(); p < n; ++p)
        up[p] = static_cast<float>(buf[p]);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c.dc->Map(w->sw, 0, D3D11_MAP_WRITE, 0, &m))) { c.ok = false; return false; }
    std::memcpy(m.pData, up.data(), up.size() * 4);
    c.dc->Unmap(w->sw, 0);
    c.dc->CopyResource(w->b0, w->sw);

    // H 趟：b0(SRV) → b1(UAV)；rx==0 直接拷贝
    if (rx >= 1) {
        const unsigned sx = kernSig(kx);
        if (sx != c.kernSig) {
            if (!uploadKern(c, kx)) { c.ok = false; return false; }
            c.kernSig = sx;
        }
        if (!(c.cbW == W && c.cbH == H && c.cbR == rx)) {
            if (!setCb(c, W, H, rx)) { c.ok = false; return false; }
            c.cbW = W; c.cbH = H; c.cbR = rx;
        }
        ID3D11ShaderResourceView* srvs[2] = {w->s0, c.kernSrv};
        c.dc->CSSetShaderResources(0, 2, srvs);
        c.dc->CSSetUnorderedAccessViews(0, 1, &w->u1, nullptr);
        c.dc->CSSetShader(c.csH, nullptr, 0);
        c.dc->CSSetConstantBuffers(0, 1, &c.cb);
        c.dc->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        // Flush 仅诊断模式需要（读回中间结果）；生产不 Flush——即时上下文
        // 命令按提交序执行，V 趟读 b1 时 H 已完成。
        { static const bool diagFlush = std::getenv("SWF_GPUBLUR_DIAG") != nullptr;
          if (diagFlush) c.dc->Flush(); }
        diagLog("afterH b1", c, *w, W, H, w->b1);
        unbindAll(c);
    } else {
        c.dc->CopyResource(w->b1, w->b0);
    }

    // V 趟：b1(SRV) → b2(UAV)；ry==0 直接拷贝
    if (ry >= 1) {
        const unsigned sy = kernSig(ky);
        if (sy != c.kernSig) {
            if (!uploadKern(c, ky)) { c.ok = false; return false; }
            c.kernSig = sy;
        }
        if (!(c.cbW == W && c.cbH == H && c.cbR == ry)) {
            if (!setCb(c, W, H, ry)) { c.ok = false; return false; }
            c.cbW = W; c.cbH = H; c.cbR = ry;
        }
        ID3D11ShaderResourceView* srvs[2] = {w->s1, c.kernSrv};
        c.dc->CSSetShaderResources(0, 2, srvs);
        c.dc->CSSetUnorderedAccessViews(0, 1, &w->u2, nullptr);
        c.dc->CSSetShader(c.csV, nullptr, 0);
        c.dc->CSSetConstantBuffers(0, 1, &c.cb);
        c.dc->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        { static const bool diagFlush = std::getenv("SWF_GPUBLUR_DIAG") != nullptr;
          if (diagFlush) c.dc->Flush(); }
        diagLog("afterV b2", c, *w, W, H, w->b2);
        unbindAll(c);
    } else {
        c.dc->CopyResource(w->b2, w->b1);
    }

    // 读回：b2 → 读回 staging → uint8（(int)(v+0.5) 对精确整数无损，防驱动微小误差）。
    // 注意：b2 是**缓冲**（非纹理），RowPitch = 整个缓冲字节数而非单行宽，
    // 必须按扁平 float4 数组读回（diag5 一致）。
    c.dc->CopyResource(w->sr, w->b2);
    if (FAILED(c.dc->Map(w->sr, 0, D3D11_MAP_READ, 0, &m))) { c.ok = false; return false; }
    const float* src = static_cast<const float*>(m.pData);
    const std::size_t n = static_cast<std::size_t>(W) * H;
    for (std::size_t p = 0; p < n; ++p) {
        std::uint8_t* o = buf + p * 4;
        for (int ch = 0; ch < 4; ++ch) {
            const float v = src[p * 4 + ch];
            int iv = static_cast<int>(v + 0.5f);
            if (iv < 0) iv = 0; else if (iv > 255) iv = 255;
            o[ch] = static_cast<std::uint8_t>(iv);
        }
    }
    c.dc->Unmap(w->sr, 0);
    QueryPerformanceCounter(&q1);
    ++g_stat.ok;
    g_stat.px += pxNow;
    const double dms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / qfq.QuadPart;
    g_stat.ms += dms;
    int b = pxNow < 4096 ? 0 : pxNow < 16384 ? 1 : pxNow < 65536 ? 2
          : pxNow < 262144 ? 3 : 4;
    ++g_stat.binCalls[b];
    g_stat.binMs[b] += dms;
    g_stat.binPx[b] += pxNow;
    if (pxNow < g_stat.minPx_) g_stat.minPx_ = pxNow;
    if (pxNow > g_stat.maxPx_) g_stat.maxPx_ = pxNow;
    return true;
}

bool blurPlane(std::uint8_t* buf, int W, int H,
               const std::vector<float>& kx, const std::vector<float>& ky) {
    // 平面 → RGBA 的 x 通道（y/z/w 恒 0：卷积线性，0 通道保持 0，
    // 故 x 通道的逐位计算与单通道 CPU 卷积完全一致）
    static thread_local std::vector<std::uint8_t> rgba;
    const std::size_t n = static_cast<std::size_t>(W) * H;
    rgba.resize(n * 4, 0);
    for (std::size_t i = 0; i < n; ++i)
        rgba[i * 4] = buf[i];
    if (!blurRgba(rgba.data(), W, H, kx, ky)) return false;
    for (std::size_t i = 0; i < n; ++i)
        buf[i] = rgba[i * 4];
    return true;
}

}  // namespace gpublur
}  // namespace swf

#else   // 非 Windows：无 GPU 路径，恒回退 CPU

namespace swf {
namespace gpublur {
bool init() { return false; }
bool blurRgba(std::uint8_t*, int, int, const std::vector<float>&, const std::vector<float>&) {
    return false;
}
bool blurPlane(std::uint8_t*, int, int, const std::vector<float>&, const std::vector<float>&) {
    return false;
}
}  // namespace gpublur
}  // namespace swf

#endif   // _WIN32
