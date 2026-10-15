#include "swf/Decompress.hpp"

#include <zlib.h>

#if defined(SWF_HAVE_LZMA)
#include <lzma.h>
#endif

namespace swf {

bool inflateZlib(const std::uint8_t* src, std::size_t srcLen, std::vector<std::uint8_t>& out) {
    z_stream zs;
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = Z_NULL;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    zs.avail_in = static_cast<uInt>(srcLen);

    if (inflateInit(&zs) != Z_OK) return false;

    const std::size_t kChunk = 64 * 1024;
    bool ok = false;
    for (;;) {
        std::size_t base = out.size();
        out.resize(base + kChunk);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + base);
        zs.avail_out = static_cast<uInt>(kChunk);

        int r = inflate(&zs, Z_NO_FLUSH);
        std::size_t produced = kChunk - static_cast<std::size_t>(zs.avail_out);
        out.resize(base + produced);

        if (r == Z_STREAM_END) { ok = true; break; }
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (r == Z_BUF_ERROR && zs.avail_in == 0) break;
    }
    inflateEnd(&zs);
    return ok;
}

bool decodeLzmaAlone(const std::uint8_t* src, std::size_t srcLen, std::vector<std::uint8_t>& out) {
#if defined(SWF_HAVE_LZMA)
    lzma_stream ls = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&ls, UINT64_MAX) != LZMA_OK) return false;

    ls.next_in = src;
    ls.avail_in = srcLen;

    const std::size_t kChunk = 64 * 1024;
    bool ok = false;
    for (;;) {
        std::size_t base = out.size();
        out.resize(base + kChunk);
        ls.next_out = out.data() + base;
        ls.avail_out = kChunk;

        lzma_ret r = lzma_code(&ls, LZMA_FINISH);
        std::size_t produced = kChunk - ls.avail_out;
        out.resize(base + produced);

        if (r == LZMA_STREAM_END) { ok = true; break; }
        if (r != LZMA_OK && r != LZMA_BUF_ERROR) break;
        if (r == LZMA_BUF_ERROR && produced == 0) break;
    }
    lzma_end(&ls);
    return ok;
#else
    (void)src;
    (void)srcLen;
    out.clear();
    return false;
#endif
}

}  // namespace swf
