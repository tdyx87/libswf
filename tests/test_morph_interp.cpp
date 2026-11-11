// MorphShape 逐记录插值测试（CTest: swfparser_morph_interp）。
//
// 背景：loading_mc(c1269) 的扇形遮罩由一串 MorphShape 组成（c1250/1252/1253/…），
// 每个 morph 让三角形的闭合边在"直边"与"鼓出的曲边"之间渐变，两个底点不动、
// 顶点移动。此前 interpolateMorph 拿**拍平后的点数**当结构一致性判据：二次曲线按
// 矢高拍平成 N 段，起始形态（直边）N=1、终止形态（曲边）N≈60，点数 4 vs 62 被判为
// "轮廓不对应"，整体退化成起始形状 —— ratio 逐帧变化但形状纹丝不动，表现为
// "loading 动画扫过 14 帧后整段冻结"。
//
// 本测试锁定修复后的不变量：
//   1) 直边↔曲边互变必须被接受并插值（不再退化）；
//   2) t=0 / t=1 精确还原起止两端的顶点；
//   3) 中间 t 的顶点线性落位，且几何与两端都不同（动画真的在动）。
#include "swf/Decoder.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

using namespace swf;

// 统计拍平后的总点数（正是旧代码误用为结构判据的那个量）。
static std::size_t flatPoints(const Shape& s) {
    std::size_t n = 0;
    for (const auto& group : s.fillPaths)
        for (const auto& ct : group) n += ct.xs.size();
    return n;
}

// 取第一个轮廓的首点（morph 只有一个填充样式时即形状的起始顶点）。
static bool firstPoint(const Shape& s, float& x, float& y) {
    if (s.fillPaths.empty() || s.fillPaths[0].empty()) return false;
    const Contour& c = s.fillPaths[0][0];
    if (c.xs.empty()) return false;
    x = c.xs[0]; y = c.ys[0];
    return true;
}

static Shape::RawRec rec(unsigned char kind, float x, float y, float cx, float cy,
                         int f0 = 0, int f1 = 0, int ln = 0, unsigned char has = 0) {
    Shape::RawRec r;
    r.kind = kind; r.x = x; r.y = y; r.cx = cx; r.cy = cy;
    r.f0 = f0; r.f1 = f1; r.ln = ln; r.has = has;
    return r;
}

int main() {
    std::printf("MorphShape 逐记录插值测试\n");

    MorphShape m;
    m.start = std::make_shared<Shape>();
    m.end   = std::make_shared<Shape>();
    FillStyle fs;                       // 实心填充，索引 1（1 基）
    m.start->fillStyles = {fs};
    m.end->fillStyles   = {fs};
    m.start->fillPaths.resize(1);  m.start->fillEdges.resize(1);
    m.end->fillPaths.resize(1);    m.end->fillEdges.resize(1);

    // 实测自 swf2.swf 的 c1252（twips）。
    // 起始：Move(-2814,2583) → (401,-295) → (425,3519) → 直边闭合回首点。
    m.start->rawRecs = {
        rec(4, 0, 0, 0, 0, 1),                 // 段首样式状态：fill0 索引 1
        rec(0, -2814, 2583, 0, 0, 1),
        rec(1,   401,  -295, 0, 0, 1),
        rec(1,   425,  3519, 0, 0, 1),
        rec(1, -2814,  2583, 0, 0, 1),
    };
    // 终止：顶点移到 (-4984,-480)，闭合边变成曲线（控制点绝对值 = 425-4554, 3519+954）。
    m.end->rawRecs = {
        rec(4, 0, 0, 0, 0, 1),
        rec(0, -4984, -480, 0, 0, 1),
        rec(1,   401, -295, 0, 0, 1),
        rec(1,   425, 3519, 0, 0, 1),
        rec(2, -4984, -480, -4129, 4473, 1),
    };

    Shape s0, s1, sh;
    const bool ok0 = interpolateMorphRecords(m, 0.0f, s0);
    const bool ok1 = interpolateMorphRecords(m, 1.0f, s1);
    const bool okh = interpolateMorphRecords(m, 0.5f, sh);

    check(ok0 && ok1 && okh, "直边↔曲边的 morph 被接受插值（不再退化成起始形状）");
    if (!ok0 || !ok1 || !okh) {
        std::printf("FAIL: 插值路径未生效 (fail=%d)\n", g_fail);
        return g_fail ? 1 : 0;
    }

    // 两端点数天然不等 —— 旧代码正是据此退化。这里断言它**不等**，
    // 从而锁定"点数不等不得成为拒绝插值的理由"。
    const std::size_t n0 = flatPoints(s0), n1 = flatPoints(s1), nh = flatPoints(sh);
    std::printf("      拍平点数: t=0 %zu, t=0.5 %zu, t=1 %zu\n", n0, nh, n1);
    check(n0 == 4, "t=0 还原起始三角形（4 点，直边闭合）");
    check(n1 > n0, "t=1 的闭合边被拍平成多段（起止点数不等，旧代码据此拒绝插值）");
    check(nh > n0 && nh < n1, "t=0.5 的拍平段数介于两端之间（曲边在鼓出过程中）");

    float x0, y0, x1, y1, xh, yh;
    check(firstPoint(s0, x0, y0) && std::fabs(x0 + 2814) < 1 && std::fabs(y0 - 2583) < 1,
          "t=0 顶点 = 起始顶点 (-2814,2583)");
    check(firstPoint(s1, x1, y1) && std::fabs(x1 + 4984) < 1 && std::fabs(y1 + 480) < 1,
          "t=1 顶点 = 终止顶点 (-4984,-480)");
    if (firstPoint(sh, xh, yh)) {
        const float ex = (-2814.f + -4984.f) * 0.5f, ey = (2583.f + -480.f) * 0.5f;
        check(std::fabs(xh - ex) < 2 && std::fabs(yh - ey) < 2,
              "t=0.5 顶点线性落位（随 ratio 连续移动，动画不再冻结）");
    } else {
        check(false, "t=0.5 顶点线性落位（随 ratio 连续移动，动画不再冻结）");
    }

    // 无原始记录时必须拒绝（退回调用方的按点插值），不能凭空造形状。
    MorphShape empty;
    empty.start = std::make_shared<Shape>();
    empty.end = std::make_shared<Shape>();
    Shape junk;
    check(!interpolateMorphRecords(empty, 0.5f, junk), "没有原始记录时拒绝插值（交回按点插值回退）");

    std::printf("%s (%d 处失败)\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
