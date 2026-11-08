// 阶段 1 架构回归测试：共享 tag 头解析、有界 TagReader、软边界 ByteView、
// Strategy/Registry/Template Method 管道、Command 表、解压输出上限。
// 以进程退出码判定成败（CTest 不设 PASS_REGULAR_EXPRESSION）。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>
#if defined(SWF_HAVE_LZMA)
#include <lzma.h>
#endif

#include "swf/Command.hpp"
#include "swf/Decompress.hpp"
#include "swf/MovieClipCommands.hpp"
#include "swf/Parser.hpp"
#include "swf/TagReader.hpp"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

void putTag(std::vector<std::uint8_t>& b, std::uint16_t code,
            std::vector<std::uint8_t> data) {
    const std::uint16_t shortLen = data.size() < 63
        ? static_cast<std::uint16_t>(data.size()) : 63;
    const std::uint16_t v = static_cast<std::uint16_t>((code << 6) | shortLen);
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    if (shortLen == 63) {
        const std::uint32_t n = static_cast<std::uint32_t>(data.size());
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xFF));
    }
    b.insert(b.end(), data.begin(), data.end());
}

struct PipeCtx {
    int handled = 0;
    int fallback = 0;
};

#if defined(SWF_HAVE_LZMA)
// 用 raw LZMA1 编码器生成压缩数据，按 ZWS 布局组装：
// UI32 CompressedSize + 5B props(lc/lp/pb + dict_size) + LZMA_RAW 流。
bool checkZwsRoundTrip() {
    std::vector<std::uint8_t> input(8192);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<std::uint8_t>((i * 7) % 251);

    lzma_options_lzma opt{};
    opt.dict_size = 1u << 23;
    opt.lc = 3; opt.lp = 0; opt.pb = 2;
    opt.mode = LZMA_MODE_NORMAL;
    opt.nice_len = 64;
    opt.mf = LZMA_MF_BT4;
    opt.depth = 0;
    lzma_filter filters[2] = {
        { LZMA_FILTER_LZMA1, &opt },
        { LZMA_VLI_UNKNOWN, nullptr }
    };
    const std::size_t bound = lzma_stream_buffer_bound(input.size());
    std::vector<std::uint8_t> raw(bound);
    std::size_t outPos = 0;
    const lzma_ret er = lzma_raw_buffer_encode(filters, nullptr, input.data(),
                                               input.size(), raw.data(), &outPos, raw.size());
    if (er != LZMA_OK || outPos == 0) {
        std::printf("  [zws] raw encode ret=%d outPos=%zu bound=%zu\n",
                    static_cast<int>(er), outPos, bound);
        return false;
    }

    const std::uint8_t props[5] = {
        static_cast<std::uint8_t>((opt.pb * 5 + opt.lp) * 9 + opt.lc),
        static_cast<std::uint8_t>(opt.dict_size & 0xFF),
        static_cast<std::uint8_t>((opt.dict_size >> 8) & 0xFF),
        static_cast<std::uint8_t>((opt.dict_size >> 16) & 0xFF),
        static_cast<std::uint8_t>((opt.dict_size >> 24) & 0xFF)
    };
    const std::uint32_t compSize = static_cast<std::uint32_t>(outPos);
    std::vector<std::uint8_t> body;
    for (int i = 0; i < 4; ++i)
        body.push_back(static_cast<std::uint8_t>((compSize >> (8 * i)) & 0xFF));
    body.insert(body.end(), props, props + 5);
    body.insert(body.end(), raw.begin(),
                raw.begin() + static_cast<std::ptrdiff_t>(outPos));

    std::vector<std::uint8_t> decoded;
    const bool ok = swf::decodeZws(body.data(), body.size(), decoded);
    if (!ok || decoded != input) {
        std::printf("  [zws] ok=%d decoded=%zu expected=%zu bodyLen=%zu rawLen=%zu\n",
                    ok ? 1 : 0, decoded.size(), input.size(), body.size(), outPos);
    }
    return ok && decoded == input;
}
#endif

bool checkInflateLimit() {
    std::vector<std::uint8_t> zeros(1u << 20, 0);
    uLongf cap = compressBound(static_cast<uLong>(zeros.size()));
    std::vector<std::uint8_t> comp(static_cast<std::size_t>(cap));
    if (compress2(comp.data(), &cap, zeros.data(),
                  static_cast<uLong>(zeros.size()), 9) != Z_OK) return false;
    comp.resize(static_cast<std::size_t>(cap));

    std::vector<std::uint8_t> small;
    if (swf::inflateZlib(comp.data(), comp.size(), small, 4096)) return false;
    std::vector<std::uint8_t> full;
    return swf::inflateZlib(comp.data(), comp.size(), full) &&
           full.size() == zeros.size();
}

}  // namespace

int main() {
    // 1) 共享 tag 头解析：短头 / 长头 / 截断 / 长度字段溢出
    {
        std::vector<std::uint8_t> b;
        putTag(b, 9, {1, 2, 3});
        swf::TagHeader th;
        check(swf::readTagHeader(b.data(), 0, b.size(), th) == swf::TagStatus::Ok,
              "short tag header parsed");
        check(th.code == 9 && th.length == 3 && th.dataOffset == 2 && th.dataEnd == 5,
              "short tag header fields");
        check(swf::readTagHeader(b.data(), 0, 1, th) == swf::TagStatus::Incomplete,
              "truncated header rejected");
        check(swf::readTagHeader(b.data(), b.size(), b.size(), th) == swf::TagStatus::Incomplete,
              "zero remaining bytes rejected");

        std::vector<std::uint8_t> lb;
        putTag(lb, 9, std::vector<std::uint8_t>(100, 0xAB));
        check(swf::readTagHeader(lb.data(), 0, lb.size(), th) == swf::TagStatus::Ok &&
              th.length == 100 && th.dataOffset == 6,
              "long tag header parsed");

        const std::uint8_t bad[] = {0x7F, 0x02, 0xFF, 0xFF, 0xFF, 0xFF};
        check(swf::readTagHeader(bad, 0, sizeof bad, th) == swf::TagStatus::Incomplete,
              "oversized length rejected without wraparound");
    }

    // 2) TagReader：以 tag 为边界，越界读返回 0 并留痕
    {
        const std::uint8_t d[] = {1, 2, 3};
        swf::TagReader r(d, sizeof d, 9);
        check(r.readUI8() == 1, "TagReader readUI8");
        check(r.readUI16() == static_cast<std::uint16_t>(2 | (3 << 8)),
              "TagReader readUI16 little endian");
        check(!r.overflow(), "TagReader in-bounds reads do not set overflow");
        check(r.readUI8() == 0 && r.overflow(), "TagReader rejects read past tag end");
        check(r.remaining() == 0, "TagReader remaining is bounded");
    }

    // 3) ByteView 软边界：收紧可读范围，但 pos() 仍是绝对偏移
    {
        const std::uint8_t d[] = {0, 1, 2, 3, 4, 5, 6, 7};
        swf::ByteView bv(d, sizeof d);
        bv.seek(2);
        bv.setLimit(4);
        check(bv.pos() == 2 && bv.limit() == 4, "soft limit keeps absolute pos");
        check(bv.readUI8() == 2 && bv.readUI8() == 3, "reads inside soft limit");
        check(bv.readUI8() == 0 && bv.overflow(), "reads beyond soft limit rejected");
    }

    // 4) Template Method：校验 -> 有界视图 -> 查表 -> fallback -> 检查 -> 推进
    {
        swf::TagRegistry<PipeCtx> reg;
        reg.add(9, [](swf::TagReader& r, PipeCtx& c, std::uint16_t) -> swf::TagStatus {
            ++c.handled;
            return r.readUI8() == 0xAA ? swf::TagStatus::Ok : swf::TagStatus::Fail;
        });
        swf::TagPipeline<PipeCtx> pipe(reg,
            [](swf::ByteView& bv, PipeCtx& c, const swf::TagHeader& th) -> swf::TagStatus {
                (void)bv;
                ++c.fallback;
                return th.code == 0 ? swf::TagStatus::Stop : swf::TagStatus::Ok;
            });

        std::vector<std::uint8_t> b;
        putTag(b, 9, {0xAA});
        putTag(b, 1, {});
        putTag(b, 0, {});
        PipeCtx c;
        std::size_t last = 0;
        const swf::TagStatus st = pipe.run(b.data(), b.size(), 0, c, 100, &last);
        check(st == swf::TagStatus::Stop, "pipeline stops at End tag");
        check(c.handled == 1 && c.fallback == 2, "registry and fallback both dispatched");
        check(last == b.size(), "pipeline advances to tag end");
    }

    // 5) Command 表：命中 / 未命中默认
    {
        struct C { int base; };
        swf::CommandRegistry<C, int, int> reg;
        reg.setUnknown(-1);
        reg.add("add", [](const C& c, int x) { return c.base + x; });
        check(reg.size() == 1, "command registered");
        check(reg.dispatch("add", C{10}, 5) == 15, "command dispatched");
        check(reg.dispatch("missing", C{10}, 5) == -1, "unknown command uses default");
    }

    // 6) Facade/Command：7 个扩展方法与动态实例
    {
        swf::Movie movie;
        std::vector<swf::DisplayItem> items;
        swf::ExtCommandContext ctx{movie, items, 1, 0.0, 0.0};
        swf::MovieClipCommands cmds;
        check(cmds.commandCount() == 7, "seven MovieClip commands registered");

        using Args = swf::MovieClipCommands::Args;
        Args a;
        a.push_back(swf::avm1::Value(std::string("mc")));
        a.push_back(swf::avm1::Value(5.0));
        const swf::avm1::Value created = cmds.call("createEmptyMovieClip", ctx, a);
        check(created.type == swf::avm1::Value::T::Object, "createEmptyMovieClip returns object");
        check(items.size() == 1 && items[0].dynamic && items[0].depth == 5 &&
              items[0].name == "mc", "dynamic item inserted by depth");

        const swf::avm1::Value bounds = cmds.call("getBounds", ctx, Args{});
        check(bounds.type == swf::avm1::Value::T::Object && bounds.obj &&
              bounds.obj->count("xMin") == 1 && bounds.obj->count("width") == 1,
              "getBounds returns object with bounds members");

        cmds.call("startDrag", ctx, Args{});
        check(cmds.isDragging(), "startDrag records state");
        cmds.call("stopDrag", ctx, Args{});
        check(!cmds.isDragging(), "stopDrag clears state");
        check(cmds.call("unknownMethod", ctx, Args{}).type ==
              swf::avm1::Value::T::Undefined, "unknown ext method returns undefined");
    }

    // 7) 解压输出上限：1MB 零数据的压缩流不能突破 maxOut
    check(checkInflateLimit(), "zlib decompression honours output cap");
#if defined(SWF_HAVE_LZMA)
    check(checkZwsRoundTrip(), "ZWS (LZMA_RAW) body decodes back to original");
#else
    std::printf("  SKIP: ZWS round-trip (SWF_HAVE_LZMA not enabled)\n");
#endif

    // 8) DefineSound 的高 4 位格式码 15 不再越界读 kFmt
    {
        std::vector<std::uint8_t> b = {'F','W','S',6, 0,0,0,0,
                                       0x00, 0x00,0x0C, 0x01,0x00};
        putTag(b, 14, {0x01, 0x00, 0xFF});
        putTag(b, 0, {});
        for (int i = 0; i < 4; ++i)
            b[4 + i] = static_cast<std::uint8_t>((b.size() >> (8 * i)) & 0xFF);

        swf::Parser parser;
        swf::ParseResult result;
        const int rc = parser.parse(b.data(), b.size(), result);
        check(rc == 2 && result.ok, "minimal SWF with DefineSound parsed");
        bool known = false;
        for (const auto& t : result.tags)
            if (t.code == 14 && t.note.find("unknown") != std::string::npos) known = true;
        check(known, "invalid sound format falls back to 'unknown' without OOB");
    }

    std::printf("\nBoundary/architecture test: %s (%d failures)\n",
                g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
