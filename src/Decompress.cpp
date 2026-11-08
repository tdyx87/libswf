#include "swf/Decompress.hpp"

#include <algorithm>
#include <limits>

#include <zlib.h>

#if defined(SWF_HAVE_LZMA)
#include <lzma.h>
#endif

namespace swf {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

std::size_t nextChunk(std::size_t produced, std::size_t maxOut) {
    if (produced >= maxOut) return 0;
    return std::min(kChunk, maxOut - produced);
}

}  // namespace

bool inflateZlib(const std::uint8_t* src, std::size_t srcLen,
                 std::vector<std::uint8_t>& out, std::size_t maxOut) {
    out.clear();
    if (!src || srcLen == 0 || maxOut == 0) return false;
    // uInt 是 32 位：更长的输入会被静默截断，宁可拒绝也不要接受不完整文件。
    if (srcLen > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) return false;

    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    zs.avail_in = static_cast<uInt>(srcLen);

    if (inflateInit(&zs) != Z_OK) return false;

    bool ok = false;
    for (;;) {
        const std::size_t chunk = nextChunk(out.size(), maxOut);
        if (chunk == 0) break;  // 到达输出上限：视为解压炸弹
        const std::size_t base = out.size();
        out.resize(base + chunk);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + base);
        zs.avail_out = static_cast<uInt>(chunk);

        const int r = inflate(&zs, Z_NO_FLUSH);
        const std::size_t produced = chunk - static_cast<std::size_t>(zs.avail_out);
        out.resize(base + produced);

        if (r == Z_STREAM_END) { ok = true; break; }
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        // 既没有产出、又没有可推进的输入时 inflate 会反复返回同一状态，
        // 必须显式退出，否则畸形流让 CPU 100% 空转。
        if (produced == 0 && (r == Z_BUF_ERROR || zs.avail_in == 0)) break;
    }
    inflateEnd(&zs);
    if (!ok) out.clear();
    return ok;
}

bool decodeZws(const std::uint8_t* src, std::size_t srcLen,
               std::vector<std::uint8_t>& out, std::size_t maxOut) {
    out.clear();
#if defined(SWF_HAVE_LZMA)
    // ZWS body: UI32 CompressedSize + 5B LZMA1 props + LZMA_RAW stream.
    if (!src || srcLen < 9 || maxOut == 0) return false;
    constexpr std::size_t kRawOff = 4 + 5;
    const std::uint32_t compressedSize = static_cast<std::uint32_t>(src[0])
        | (static_cast<std::uint32_t>(src[1]) << 8)
        | (static_cast<std::uint32_t>(src[2]) << 16)
        | (static_cast<std::uint32_t>(src[3]) << 24);
    // 各实现对该字段是否包含 props 口径不一，只做宽松上界校验，
    // 实际输入长度以 srcLen 为准，避免误切压缩流。
    if (compressedSize != 0 && static_cast<std::size_t>(compressedSize) > srcLen - 4)
        return false;

    // raw 解码器只需要 lc/lp/pb/dict_size，无需整套编码预设。
    lzma_options_lzma opt{};
    int p = src[4];                       // (pb * 5 + lp) * 9 + lc
    opt.lc = static_cast<std::uint32_t>(p % 9); p /= 9;
    opt.lp = static_cast<std::uint32_t>(p % 5); p /= 5;
    opt.pb = static_cast<std::uint32_t>(p);
    if (opt.pb > 4 || opt.lc + opt.lp > 4) return false;
    const std::uint32_t dict = static_cast<std::uint32_t>(src[5])
        | (static_cast<std::uint32_t>(src[6]) << 8)
        | (static_cast<std::uint32_t>(src[7]) << 16)
        | (static_cast<std::uint32_t>(src[8]) << 24);
    opt.dict_size = dict ? dict : 4096;
    // 限制解码器字典内存：恶意 props 里的巨数字典不应直接申请数 GB。
    constexpr std::uint64_t kMemLimit = 128ull * 1024ull * 1024ull;
    if (opt.dict_size > kMemLimit) return false;

    lzma_filter filters[2] = {
        { LZMA_FILTER_LZMA1, &opt },
        { LZMA_VLI_UNKNOWN, nullptr }
    };
    lzma_stream ls = LZMA_STREAM_INIT;
    if (lzma_raw_decoder(&ls, filters) != LZMA_OK) return false;

    ls.next_in = src + kRawOff;
    ls.avail_in = srcLen - kRawOff;

    bool ok = false;
    for (;;) {
        const std::size_t chunk = nextChunk(out.size(), maxOut);
        if (chunk == 0) break;
        const std::size_t base = out.size();
        out.resize(base + chunk);
        ls.next_out = out.data() + base;
        ls.avail_out = chunk;

        const lzma_ret r = lzma_code(&ls, LZMA_FINISH);
        const std::size_t produced = chunk - ls.avail_out;
        out.resize(base + produced);

        if (r == LZMA_STREAM_END) { ok = true; break; }
        // ZWS 的 LZMA_RAW 流不带 EOS marker，流长度由 CompressedSize 界定：
        // 输入耗尽后的 LZMA_BUF_ERROR 是正常结束，不该判为解码失败。
        if (ls.avail_in == 0 && (r == LZMA_BUF_ERROR || produced == 0)) {
            ok = !out.empty();
            break;
        }
        if (r != LZMA_OK) break;
    }
    lzma_end(&ls);
    if (!ok) out.clear();
    return ok;
#else
    (void)src;
    (void)srcLen;
    (void)maxOut;
    return false;
#endif
}

}  // namespace swf
