// 方案 B GPU 模糊独立参照测试：确定性缓冲 + 逐字复制的 CPU H/V 参考（d==1 路径）
// 对比 gpublur::blurRgba / blurPlane，报告 max-delta 与首个错位位置。
// 快速定位 bug（无需 1600 步 A/B）。
#include "swf/GpuBlur.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static std::uint8_t clampU8(int v) { return (v < 0) ? 0 : (v > 255) ? 255 : (std::uint8_t)v; }

// —— 与 Render.cpp gaussianKernel 逐字一致 ——
static std::vector<float> gaussianKernel(float sigma) {
    if (sigma < 0.01f) return std::vector<float>{1.0f};
    const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    const double inv2s2 = 1.0 / (2.0 * static_cast<double>(sigma) * sigma);
    double sum = 0.0;
    for (int i = -r; i <= r; ++i) {
        const double v = std::exp(-static_cast<double>(i) * i * inv2s2);
        k[static_cast<std::size_t>(i + r)] = static_cast<float>(v);
        sum += v;
    }
    const float in = static_cast<float>(1.0 / sum);
    for (auto& x : k) x *= in;
    return k;
}

// —— 与 Render.cpp blurBufferCore d==1 H/V 路径逐字一致（RGBA）——
static void cpuHvRgba(std::uint8_t* work, int W, int H,
                      const std::vector<float>& kx, const std::vector<float>& ky,
                      std::vector<std::uint8_t>& tmp) {
    const int rx = static_cast<int>(kx.size()) / 2;
    const int ry = static_cast<int>(ky.size()) / 2;
    tmp.assign(static_cast<std::size_t>(W) * H * 4, 0);
    std::uint8_t* t = tmp.data();
    if (rx >= 1) {
        for (int y = 0; y < H; ++y) {
            const std::uint8_t* row = &work[static_cast<std::size_t>(y) * W * 4];
            std::uint8_t* o = &t[static_cast<std::size_t>(y) * W * 4];
            for (int x = 0; x < W; ++x) {
                float acc[4] = {0,0,0,0};
                for (int i = -rx; i <= rx; ++i) {
                    const int xx = std::min(W-1, std::max(0, x+i));
                    const float kv = kx[static_cast<std::size_t>(i+rx)];
                    const std::uint8_t* s = &row[xx*4];
                    for (int c=0;c<4;++c) acc[c]+=s[c]*kv;
                }
                for (int c=0;c<4;++c) o[x*4+c]=clampU8(static_cast<int>(acc[c]+0.5f));
            }
        }
    } else std::memcpy(t, work, static_cast<std::size_t>(W)*H*4);
    if (ry >= 1) {
        for (int y = 0; y < H; ++y) {
            std::uint8_t* o = &work[static_cast<std::size_t>(y) * W * 4];
            for (int x = 0; x < W; ++x) {
                float acc[4] = {0,0,0,0};
                for (int j = -ry; j <= ry; ++j) {
                    const int yy = std::min(H-1, std::max(0, y+j));
                    const float kv = ky[static_cast<std::size_t>(j+ry)];
                    const std::uint8_t* s = &t[(static_cast<std::size_t>(yy)*W+x)*4];
                    for (int c=0;c<4;++c) acc[c]+=s[c]*kv;
                }
                for (int c=0;c<4;++c) o[x*4+c]=clampU8(static_cast<int>(acc[c]+0.5f));
            }
        }
    } else std::memcpy(work, t, static_cast<std::size_t>(W)*H*4);
}

// —— 与 Render.cpp blurPlane d==1 H/V 路径逐字一致（单通道）——
static void cpuHvPlane(std::uint8_t* work, int W, int H,
                       const std::vector<float>& kx, const std::vector<float>& ky,
                       std::vector<std::uint8_t>& tmp) {
    const int rx = static_cast<int>(kx.size()) / 2;
    const int ry = static_cast<int>(ky.size()) / 2;
    tmp.assign(static_cast<std::size_t>(W) * H, 0);
    std::uint8_t* t = tmp.data();
    if (rx >= 1) {
        for (int y = 0; y < H; ++y) {
            const std::uint8_t* row = &work[static_cast<std::size_t>(y) * W];
            std::uint8_t* o = &t[static_cast<std::size_t>(y) * W];
            for (int x = 0; x < W; ++x) {
                float acc = 0.0f;
                for (int i = -rx; i <= rx; ++i) {
                    const int xx = std::min(W-1, std::max(0, x+i));
                    acc += row[xx] * kx[static_cast<std::size_t>(i+rx)];
                }
                o[x] = clampU8(static_cast<int>(acc + 0.5f));
            }
        }
    } else std::memcpy(t, work, static_cast<std::size_t>(W)*H);
    if (ry >= 1) {
        for (int y = 0; y < H; ++y) {
            std::uint8_t* o = &work[static_cast<std::size_t>(y) * W];
            for (int x = 0; x < W; ++x) {
                float acc = 0.0f;
                for (int j = -ry; j <= ry; ++j) {
                    const int yy = std::min(H-1, std::max(0, y+j));
                    acc += t[static_cast<std::size_t>(yy)*W+x] * ky[static_cast<std::size_t>(j+ry)];
                }
                o[x] = clampU8(static_cast<int>(acc + 0.5f));
            }
        }
    } else std::memcpy(work, t, static_cast<std::size_t>(W)*H);
}

static void report(const char* tag, const std::vector<std::uint8_t>& a,
                   const std::vector<std::uint8_t>& b, int W, int H, int ch) {
    long nmis = 0; int maxd = 0; int fx=-1, fy=-1, fc=-1; long bydelta[257] = {0};
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int c = 0; c < ch; ++c) {
        const int d = std::abs(static_cast<int>(a[static_cast<std::size_t>(y)*W*ch + x*ch + c]) -
                               static_cast<int>(b[static_cast<std::size_t>(y)*W*ch + x*ch + c]));
        if (d) {
            ++nmis;
            if (d > maxd) { maxd = d; fx = x; fy = y; fc = c; }
            bydelta[d]++;
        }
    }
    std::printf("  [%s] W=%d H=%d mismatches=%ld max=%d first(x,y,c)=(%d,%d,%d)\n",
                tag, W, H, nmis, maxd, fx, fy, fc);
    long top = 0; for (int d = 1; d <= 256; ++d) top += bydelta[d];
    if (nmis && top) {
        std::printf("      delta>0 像素分布(前12档):");
        int shown = 0;
        for (int d = 1; d <= 256 && shown < 12; ++d)
            if (bydelta[d]) { std::printf(" %d:%ld", d, bydelta[d]); ++shown; }
        std::printf("\n");
    }
}

int main() {
    // 参考测试用于验证 GPU 计算路径的逐位一致，强制启用 GPU（绕过自动探测的回退）
    _putenv_s("SWF_GPU_BLUR_FORCE", "1");
    if (const char* dg = std::getenv("GPUBLUR_DIAG")) {
        // 单点脉冲诊断：(32,32) = 255，其余 0。打印 GPU/CPU 结果核分布。
        const int W = 64, H = 64;
        std::vector<std::uint8_t> base(static_cast<std::size_t>(W) * H * 4, 0);
        for (int c = 0; c < 4; ++c) base[(static_cast<std::size_t>(32) * W + 32) * 4 + c] = 255;
        const std::vector<float> kx = gaussianKernel(1.0f);
        const std::vector<float> ky = gaussianKernel(1.0f);
        std::vector<std::uint8_t> ref = base, gpu = base, tmp;
        cpuHvRgba(ref.data(), W, H, kx, ky, tmp);
        const bool ok = swf::gpublur::blurRgba(gpu.data(), W, H, kx, ky);
        std::printf("diag ok=%d |kx|=%zu\n", ok ? 1 : 0, kx.size());
        long sref = 0, sgpu = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            sref += ref[(static_cast<std::size_t>(y) * W + x) * 4 + 3];
            sgpu += gpu[(static_cast<std::size_t>(y) * W + x) * 4 + 3];
        }
        std::printf("alpha mass: cpu=%ld gpu=%ld (expect ~255)\n", sref, sgpu);
        // 细探：33..41 行/列（脉冲 32 为中心），每行打印 cpu 行与 gpu 行
        std::printf("fine 41x41 (x=33..41); each row: 'C:' cpu, 'G:' gpu alpha\n");
        for (int y = 33; y <= 41; ++y) {
            std::printf("y=%d C:", y);
            for (int x = 33; x <= 41; ++x)
                std::printf(" %3d", ref[(static_cast<std::size_t>(y) * W + x) * 4 + 3]);
            std::printf("\n      G:");
            for (int x = 33; x <= 41; ++x)
                std::printf(" %3d", gpu[(static_cast<std::size_t>(y) * W + x) * 4 + 3]);
            std::printf("\n");
        }
        // 行/列剖面（全宽）
        std::printf("row profile y=32 (x=24..40): ");
        for (int x = 24; x <= 40; ++x) {
            int a = ref[(static_cast<std::size_t>(32) * W + x) * 4 + 3];
            int b = gpu[(static_cast<std::size_t>(32) * W + x) * 4 + 3];
            std::printf("%d", a == b ? a : (a * 1000 + b));
            if (x < 40) std::printf(",");
        }
        std::printf("\n");
        std::printf("col profile x=32 (y=24..40): ");
        for (int y = 24; y <= 40; ++y) {
            int a = ref[(static_cast<std::size_t>(y) * W + 32) * 4 + 3];
            int b = gpu[(static_cast<std::size_t>(y) * W + 32) * 4 + 3];
            std::printf("%d", a == b ? a : (a * 1000 + b));
            if (y < 40) std::printf(",");
        }
        std::printf("\n");
        std::printf("(纯数字=两路一致; a*1000+b=cpu=a gpu=b)\n");
        return 0;
    }
    const int W = 320, H = 200;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> u0(0, 255);
    std::vector<std::uint8_t> base(static_cast<std::size_t>(W) * H * 4);
    for (auto& v : base) v = static_cast<std::uint8_t>(u0(rng));
    // 叠一个实心块（测试大值饱和与边缘 clamp）
    for (int y = 40; y < 80; ++y) for (int x = 40; x < 120; ++x) {
        std::uint8_t* p = &base[(static_cast<std::size_t>(y)*W+x)*4];
        p[0]=200; p[1]=100; p[2]=50; p[3]=255;
    }

    struct Case { const char* name; float sx; float sy; };
    const Case cases[] = {
        {"sigma=1  (r=3)", 1.0f, 1.0f},
        {"sigma=5  (r=15)", 5.0f, 5.0f},
        {"sigma=8  (r=24)", 8.0f, 8.0f},
        {"sigma=3/2 (anis)", 3.0f, 2.0f},
        {"sigma=1/0 (H-only)", 1.0f, 0.0f},
    };

    for (const Case& cs : cases) {
        const std::vector<float> kx = gaussianKernel(cs.sx);
        const std::vector<float> ky = gaussianKernel(cs.sy);
        std::printf("case %s  |kx|=%zu |ky|=%zu\n", cs.name, kx.size(), ky.size());

        // RGBA
        {
            std::vector<std::uint8_t> ref = base;
            std::vector<std::uint8_t> tmp;
            cpuHvRgba(ref.data(), W, H, kx, ky, tmp);
            std::vector<std::uint8_t> gpu = base;
            const bool ok = swf::gpublur::blurRgba(gpu.data(), W, H, kx, ky);
            std::printf("  blurRgba ok=%d\n", ok ? 1 : 0);
            if (ok) report("Rgba vs CPU", ref, gpu, W, H, 4);
        }
        // Plane
        {
            std::vector<std::uint8_t> plane(static_cast<std::size_t>(W) * H);
            for (int i = 0; i < W * H; ++i) plane[i] = base[static_cast<std::size_t>(i) * 4 + 3];
            std::vector<std::uint8_t> ref = plane;
            std::vector<std::uint8_t> tmp;
            cpuHvPlane(ref.data(), W, H, kx, ky, tmp);
            std::vector<std::uint8_t> gpu = plane;
            const bool ok = swf::gpublur::blurPlane(gpu.data(), W, H, kx, ky);
            std::printf("  blurPlane ok=%d\n", ok ? 1 : 0);
            if (ok) report("Plane vs CPU", ref, gpu, W, H, 1);
        }
    }
    std::printf("done\n");
    return 0;
}
