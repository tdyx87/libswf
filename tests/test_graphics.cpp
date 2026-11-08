// AS3 Graphics 绘制 API 修复回归测试：VM 端命令记录 + 渲染端像素回放，双端验证。
// 此前 Graphics 整组方法（beginFill/drawRect/...）为 no-op，AVM2 动态绘制内容完全不可见。
// CTest: swfparser_graphics。修复记录见 docs/FIXES_LOG.md（FIX-2026-001）。
#include "swf/Abc.hpp"
#include "swf/Decoder.hpp"
#include "swf/Fvm.hpp"
#include "swf/Render.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace f = swf::fvm;
using f::ObjKind;
using f::Value;
using f::VKind;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!cond) ++g_fail;
}

int main() {
    // ---- 1) VM 端：graphics getter 缓存 + 绘制命令按序记录 ----
    // 实例须经 constructObject 构造（挂上 ClassDef，成员/getter 解析才生效；
    // 与 test_avm2_runtime 的 make() 模式一致）。
    swf::Movie movie;
    swf::abc::AbcFile abc;
    f::Vm vm(abc, movie);
    vm.run();   // 注册全局内建类/函数（globals_ 在 run() 中初始化）
    auto globalOf = [&](const char* n) -> Value {
        auto it = vm.globalsObj()->props.find(n);
        return it == vm.globalsObj()->props.end() ? Value::undefined() : it->second;
    };
    Value spriteV = vm.constructObject(globalOf("Sprite"), {});
    auto sprite = spriteV.obj;
    check(sprite != nullptr && sprite->display != nullptr,
          "constructObject(Sprite) 携带 DisplayObject");
    if (!sprite || !sprite->display) {
        std::printf("FAILED (%d)\n", g_fail);
        return 1;
    }

    Value gv = vm.getProperty(sprite, "graphics");
    check(gv.kind == VKind::Object && gv.obj != nullptr, "sprite.graphics 返回 Graphics 实例");
    if (!(gv.kind == VKind::Object && gv.obj)) {
        std::printf("FAILED (%d)\n", g_fail);
        return 1;
    }
    Value gv2 = vm.getProperty(sprite, "graphics");
    check(gv2.kind == VKind::Object && gv2.obj == gv.obj,
          "重复访问 graphics 返回同一实例（Flash 语义缓存）");

    vm.callMethod(gv.obj, "beginFill", {Value::intg(0xFF0000), Value::num(1.0)});
    vm.callMethod(gv.obj, "drawRect",
                  {Value::num(10), Value::num(20), Value::num(50), Value::num(40)});
    vm.callMethod(gv.obj, "endFill", {});
    auto& cmds = *sprite->display->graphics;
    check(cmds.size() == 3, "beginFill/drawRect/endFill 按序记录 3 条命令");
    check(cmds[0].op == f::GraphicsCmd::Op::BeginFill && cmds[0].color == 0xFFFF0000u,
          "beginFill 颜色+alpha 转为 ARGB(0xFFFF0000)");
    check(cmds[1].op == f::GraphicsCmd::Op::Rect && cmds[1].x == 10 &&
              cmds[1].y == 20 && cmds[1].w == 50 && cmds[1].h == 40,
          "drawRect 参数按序记录");
    check(cmds[2].op == f::GraphicsCmd::Op::EndFill, "endFill 提交填充组");

    vm.callMethod(gv.obj, "lineStyle",
                  {Value::num(2), Value::intg(0x00FF00), Value::num(0.5)});
    check(cmds.size() == 4 && cmds[3].op == f::GraphicsCmd::Op::LineStyle &&
              cmds[3].a == 2 && cmds[3].color == 0x8000FF00u,
          "lineStyle 记录厚度与 ARGB（含 0.5 alpha）");

    vm.callMethod(gv.obj, "drawCircle", {Value::num(5), Value::num(6), Value::num(7)});
    check(cmds.size() == 5 && cmds[4].op == f::GraphicsCmd::Op::Circle &&
              cmds[4].r == 7, "drawCircle 记录圆心与半径");

    vm.callMethod(gv.obj, "clear", {});
    check(sprite->display->graphics->empty(), "clear() 清空全部命令");

    // ---- 2) 渲染端：Graphics 命令回放为像素（fillEdgesWinding 非零环绕）----
    {
        swf::Movie rmovie;
        auto d = std::make_shared<f::DisplayObject>();
        d->kind = ObjKind::Shape;
        d->graphics = std::make_shared<std::vector<f::GraphicsCmd>>();
        f::GraphicsCmd bf;
        bf.op = f::GraphicsCmd::Op::BeginFill;
        bf.color = 0xFFFF0000;
        d->graphics->push_back(bf);
        f::GraphicsCmd rc;
        rc.op = f::GraphicsCmd::Op::Rect;
        rc.x = 10; rc.y = 10; rc.w = 40; rc.h = 40;
        d->graphics->push_back(rc);
        f::GraphicsCmd ef;
        ef.op = f::GraphicsCmd::Op::EndFill;
        d->graphics->push_back(ef);

        const int W = 80, H = 80;
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(W) * H * 4, 0);
        // 根矩阵：局部 twips -> 设备像素（1/20），与主渲染管线一致。
        swf::Matrix root;
        root.sx = root.sy = 1.0f / 20.0f;
        swf::renderRuntimeObject(d, root, rmovie, buf.data(), W, H, 0);
        auto px = [&](int x, int y) -> const std::uint8_t* {
            return buf.data() + (static_cast<std::size_t>(y) * W + x) * 4;
        };
        const auto* mid = px(30, 30);   // 矩形内部（局部像素 (30,30)）
        check(mid[0] == 255 && mid[1] == 0 && mid[2] == 0 && mid[3] == 255,
              "矩形内部为纯填充色（红）");
        const auto* corner = px(2, 2);  // 矩形外
        check(corner[3] == 0, "矩形外部保持透明");

        // 圆形：圆心 (40,40) r=15 —— 圆内命中、圆外未命中
        d->graphics->clear();
        f::GraphicsCmd gbf;
        gbf.op = f::GraphicsCmd::Op::BeginFill;
        gbf.color = 0xFF00FF00;
        d->graphics->push_back(gbf);
        f::GraphicsCmd cf;
        cf.op = f::GraphicsCmd::Op::Circle;
        cf.x = 40; cf.y = 40; cf.r = 15;
        cf.color = 0xFF00FF00;
        d->graphics->push_back(cf);
        d->graphics->push_back(ef);
        std::fill(buf.begin(), buf.end(), 0);
        swf::renderRuntimeObject(d, root, rmovie, buf.data(), W, H, 0);
        const auto* inC = px(40, 40);
        check(inC[1] == 255 && inC[0] == 0 && inC[3] == 255, "圆形内部为纯填充色（绿）");
        const auto* outC = px(40, 10);
        check(outC[3] == 0, "圆形外部保持透明");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
