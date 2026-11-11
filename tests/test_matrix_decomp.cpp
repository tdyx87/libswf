// 时间轴放置矩阵的分解/重建一致性测试（CTest: swfparser_matrix_decomp）。
//
// 背景：PlaceObject 携带的 2x2 矩阵在运行时被分解成 scaleX/scaleY/rotation
// 三自由度近似（hasFull=false 时渲染器用它重建）。SWF 矩阵布局为
//     [[ScaleX, RotateSkew1],
//      [RotateSkew0, ScaleY]]
// RotateSkew0 可能含错切分量，不能当作旋转角；Flash 语义为
//     a = cos·sx,  b = -sin·sx,
//     c = sin·sy,  d =  cos·sy
// 故旋转角 = atan2(-r0, sx)，scaleX = hypot(sx, r0)。
//
// 此前实现误用 atan2(r1, sx) 且 scaleX = hypot(sx, r1)：对带镜像/错切的矩阵
// （如拍手动画的两只手掌/两条手臂）分解后重建的矩阵与原矩阵不符，导致手掌
// 被甩到腹部、手臂倾斜。本测试锁定"分解→重建必须还原原矩阵"这一不变量。
#include "swf/Decoder.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

// 渲染器（Render.cpp displayLocalMatrix / runtimeLocalMatrix）的重建公式：
//   a = scaleX·cos(rot),  b = -scaleX·sin(rot),
//   c = scaleY·sin(rot),  d =  scaleY·cos(rot)
static void rebuild(double scaleX, double scaleY, double rotation,
                    double& sx, double& r0, double& r1, double& sy) {
    double rad = rotation * 3.14159265358979323846 / 180.0;
    double ca = std::cos(rad), sa = std::sin(rad);
    sx = scaleX * ca;
    r0 = -scaleX * sa;
    r1 = scaleY * sa;
    sy = scaleY * ca;
}

static void apply(double sx, double r0, double r1, double sy,
                  double x, double y, double& ox, double& oy) {
    ox = sx * x + r0 * y;
    oy = r1 * x + sy * y;
}

// 断言：分解→重建后，图形上的点仍落在同一位置
static void checkRoundTrip(const char* label, double sx, double r0, double r1, double sy,
                           double px, double py, double tol) {
    swf::Matrix m;
    m.sx = static_cast<float>(sx); m.r0 = static_cast<float>(r0);
    m.r1 = static_cast<float>(r1); m.sy = static_cast<float>(sy);
    double scaleX = 0, scaleY = 0, rotation = 0;
    m.decompose(scaleX, scaleY, rotation);
    double nsx, nr0, nr1, nsy;
    rebuild(scaleX, scaleY, rotation, nsx, nr0, nr1, nsy);
    double ax, ay, bx, by;
    apply(sx, r0, r1, sy, px, py, ax, ay);
    apply(nsx, nr0, nr1, nsy, px, py, bx, by);
    double err = std::fabs(ax - bx) + std::fabs(ay - by);
    std::printf("  [%s] %s (err=%.6f)\n", err <= tol ? "PASS" : "FAIL", label, err);
    if (err > tol) {
        ++g_fail;
        std::printf("        orig lin=[%.4f %.4f; %.4f %.4f] -> (%.2f,%.2f)\n", sx, r0, r1, sy, ax, ay);
        std::printf("        recomp lin=[%.4f %.4f; %.4f %.4f] -> (%.2f,%.2f)\n", nsx, nr0, nr1, nsy, bx, by);
    }
}

int main() {
    std::printf("matrix decomposition round-trip test\n");

    // 拍手动画的真实数据（swf2.swf DefineSprite 1042 第 200 帧，JPEXS 逐值核对过）：
    //   d910 手掌：镜像矩阵 det<0，RotateSkew0/RotateSkew1 近乎相等 → 旧实现旋转角算错
    //   d912 手掌：纯旋转矩阵
    //   d792 手臂：镜像矩阵
    //   d797 手臂：纯旋转矩阵
    checkRoundTrip("palm d910 (mirrored)", -0.377, -0.6529, -0.6529, 0.377, 62.0, 75.0, 1e-3);
    checkRoundTrip("palm d912 (rotated)", 0.458, 0.5977, -0.5977, 0.458, 62.0, 75.0, 1e-3);
    checkRoundTrip("arm  d792 (mirrored)", -0.964, 0.2594, 0.2594, 0.964, 85.0, 122.0, 1e-3);
    checkRoundTrip("arm  d797 (rotated)", 0.965, -0.2549, 0.2549, 0.965, 85.0, 122.0, 1e-3);

    // 常见情形
    checkRoundTrip("pure mirror X", -1.0, 0.0, 0.0, 1.0, 40.0, 25.0, 1e-3);
    checkRoundTrip("pure scale", 0.5, 0.0, 0.0, 2.0, 40.0, 25.0, 1e-3);
    checkRoundTrip("identity", 1.0, 0.0, 0.0, 1.0, 40.0, 25.0, 1e-3);
    checkRoundTrip("45deg rotate", 1.0, -0.7071, 0.7071, 1.0, 40.0, 25.0, 1e-3);

    // 镜像时 scaleY 必须保留负号（否则镜像退化成 180° 旋转，图形翻到错误一侧）
    {
        swf::Matrix m;
        m.sx = -0.377f; m.r0 = -0.6529f; m.r1 = -0.6529f; m.sy = 0.377f;
        double sX = 0, sY = 0, rot = 0;
        m.decompose(sX, sY, rot);
        check(sY < 0, "mirrored matrix keeps negative scaleY sign");
    }
    {
        swf::Matrix m;
        m.sx = 0.458f; m.r0 = 0.5977f; m.r1 = -0.5977f; m.sy = 0.458f;
        double sX = 0, sY = 0, rot = 0;
        m.decompose(sX, sY, rot);
        check(sY > 0, "non-mirrored matrix keeps positive scaleY sign");
    }

    // 三自由度模型无法表达真正的错切矩阵（r0 != r1）：这正是必须保留完整 2x2
    // （DisplayObject::hasFull）的原因。此处只验证分解不产生 NaN/Inf。
    {
        swf::Matrix m;
        m.sx = 0.8f; m.r0 = 0.3f; m.r1 = 0.1f; m.sy = 0.9f;
        double sX = 0, sY = 0, rot = 0;
        m.decompose(sX, sY, rot);
        bool finite = std::isfinite(sX) && std::isfinite(sY) && std::isfinite(rot);
        check(finite, "skewed matrix decomposes to finite values");
    }

    std::printf("\nmatrix decomposition test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
