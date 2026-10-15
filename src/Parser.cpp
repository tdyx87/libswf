#include "swf/Parser.hpp"

#include <cstdio>
#include <cstring>

#include "swf/BitReader.hpp"
#include "swf/Decompress.hpp"

namespace swf {

std::size_t ParseResult::frameTagCount() const {
    std::size_t n = 0;
    for (const auto& t : tags)
        if (tagGroup(t.code) == TagGroup::Execution) ++n;
    return n;
}

std::size_t ParseResult::definitionTagCount() const {
    std::size_t n = 0;
    for (const auto& t : tags)
        if (tagGroup(t.code) == TagGroup::Definition) ++n;
    return n;
}

// sub_47A550：UI16 -> code = v>>6, len = v&0x3F；len==63 时再读 UI32
// 数据不足则回滚游标并返回 -1；越界返回 -2
int Parser::readTagHeader(const std::uint8_t* data, std::size_t pos, std::size_t avail,
                          TagHeader& th) {
    if (pos + 2 > avail) {
        if (pos + 2 > avail && avail >= pos) return -1;  // 需要更多数据
        return -2;
    }
    std::uint16_t v = static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8));
    std::uint32_t len = v & 0x3F;
    std::size_t cursor = pos + 2;

    if (len == 63) {
        if (cursor + 4 > avail) return -1;  // 回滚由调用方保持 pos 不变
        len = static_cast<std::uint32_t>(data[cursor])
            | (static_cast<std::uint32_t>(data[cursor + 1]) << 8)
            | (static_cast<std::uint32_t>(data[cursor + 2]) << 16)
            | (static_cast<std::uint32_t>(data[cursor + 3]) << 24);
        cursor += 4;
    }
    if (cursor + len > avail) return -1;

    th.code = static_cast<std::uint16_t>(v >> 6);
    th.length = len;
    th.dataOffset = cursor;
    th.dataEnd = cursor + len;
    return 0;
}

// sub_48BA50 的标签分派（此处做浅解析，仅还原逆向确认的字段）
void Parser::dispatch(const std::uint8_t* data, std::size_t avail, TagInfo& info) {
    const std::size_t off = info.dataOffset;
    const std::uint32_t len = info.length;
    if (off + len > avail) return;

    ByteView bv(data, off + len);
    bv.seek(off);

    switch (info.code) {
        case 9: {  // SetBackgroundColor：sub_47A7A0 读 RGB
            auto c = bv.readRGBA(false);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "RGB(#%02X%02X%02X)", c.r, c.g, c.b);
            info.note = buf;
            break;
        }
        case 2: case 22: case 32: case 83: {  // DefineShape*
            info.shapeId = bv.readUI16();
            BitReader br(bv);
            info.shapeBounds = readRect(br);
            br.finish();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "id=%u bounds=%d,%d,%d,%d",
                          info.shapeId, info.shapeBounds.xMin, info.shapeBounds.xMax,
                          info.shapeBounds.yMin, info.shapeBounds.yMax);
            info.note = buf;
            break;
        }
        case 3: {  // FreeCharacter：只读 UI16 的 CharacterID
            info.shapeId = bv.readUI16();
            info.note = "free id=" + std::to_string(info.shapeId);
            break;
        }
        case 69: {  // FileAttributes
            std::uint32_t flags = bv.readUI32();
            info.note = "flags=0x" + std::to_string(flags);
            break;
        }
        case 65: {  // ScriptLimits
            std::uint16_t depth = bv.readUI16();
            std::uint16_t timeout = bv.readUI16();
            info.note = "depth=" + std::to_string(depth) +
                        " timeout=" + std::to_string(timeout) + "s";
            break;
        }
        case 76: {  // SymbolClass：UI16 count + (UI16 tag, 名称) 序列
            std::uint16_t count = bv.readUI16();
            info.note = "symbols=" + std::to_string(count);
            break;
        }
        case 82: {  // DoABC：逆向中仅登记位置，解码推迟到 ShowFrame 前
            info.note = "abc block (" + std::to_string(len) + " bytes, deferred)";
            break;
        }
        case 93: {  // EnableTelemetry：UI16 保留 + 32 字节口令/哈希
            bv.readUI16();
            info.note = "telemetry password hash (" + std::to_string(len) + " bytes)";
            break;
        }
        case 43: {  // FrameLabel
            info.note = "frame label at " + std::to_string(off);
            break;
        }
        default:
            info.note = std::to_string(len) + " bytes";
            break;
    }
}

int Parser::parse(const std::uint8_t* data, std::size_t size, ParseResult& out) {
    out = ParseResult{};
    if (size < 8) {
        out.error = "file too small (need 8-byte header)";
        return -1;
    }

    // --- sub_490330：头解析 ---
    out.header.signature[0] = static_cast<char>(data[0]);
    out.header.signature[1] = static_cast<char>(data[1]);
    out.header.signature[2] = static_cast<char>(data[2]);
    out.header.signature[3] = '\0';
    out.header.version = data[3];
    out.header.fileLength = static_cast<std::uint32_t>(data[4])
                          | (static_cast<std::uint32_t>(data[5]) << 8)
                          | (static_cast<std::uint32_t>(data[6]) << 16)
                          | (static_cast<std::uint32_t>(data[7]) << 24);

    // sig[0] ∈ {'F','C','Z'} && sig[1]=='W' && sig[2]=='S'
    if (!out.header.valid()) {
        out.error = "bad SWF signature";
        return -1;
    }
    if (out.header.version == 0) {
        out.error = "invalid SWF version 0";
        return -1;
    }
    if (out.header.fileLength < 8) {
        out.error = "invalid FileLength";
        return -1;
    }

    if (data[0] == 'C') out.header.compression = Compression::Zlib;
    else if (data[0] == 'Z') out.header.compression = Compression::Lzma;
    else out.header.compression = Compression::Uncompressed;

    // --- 解压：body 从 offset 8 开始 ---
    std::vector<std::uint8_t> plain;
    if (out.header.compression == Compression::Uncompressed) {
        plain.assign(data + 8, data + size);
    } else {
        bool ok = false;
        if (out.header.compression == Compression::Zlib) {
            ok = inflateZlib(data + 8, size - 8, plain);
            if (!ok) out.error = "zlib inflate failed";
        } else {
            ok = decodeLzmaAlone(data + 8, size - 8, plain);
            if (!ok) out.error = "LZMA decode failed or not compiled in";
        }
        if (!ok) return -1;
    }

    // 组装“展开后”的缓冲：8 字节头（签名改回 FWS）+ 解压后的 body
    std::vector<std::uint8_t> full;
    full.reserve(8 + plain.size());
    full.push_back('F');
    full.push_back('W');
    full.push_back('S');
    full.push_back(out.header.version);
    for (int i = 0; i < 4; ++i)
        full.push_back(static_cast<std::uint8_t>((out.header.fileLength >> (8 * i)) & 0xFF));
    full.insert(full.end(), plain.begin(), plain.end());

    const std::uint8_t* base = full.data();
    const std::size_t avail = full.size();
    out.bodySize = avail;

    // --- body 头部：RECT + UI16 frameRate + UI16 frameCount ---
    std::size_t pos = 8;
    {
        ByteView bv(base, avail);
        bv.seek(8);
        BitReader br(bv);
        out.body.frameSize = readRect(br);
        br.finish();  // 退回 refill 预读的整字节
        out.body.frameRateRaw = bv.readUI16();  // 8.8 定点
        out.body.frameCount = bv.readUI16();
        pos = bv.pos();  // 标签循环从这里开始
    }

    // --- sub_48C230：标签循环 ---
    for (;;) {
        if (pos + 2 > avail) return out.tags.empty() ? -1 : 1;  // 1 = 数据不足待续
        TagHeader th;
        int rc = readTagHeader(base, pos, avail, th);
        if (rc < 0) return rc;
        if (th.code == 0) {  // End
            out.tags.push_back(TagInfo{0, th.length, th.dataOffset, "end of tags", 0, Rect{}});
            out.ok = true;
            return 2;
        }
        TagInfo info;
        info.code = th.code;
        info.length = th.length;
        info.dataOffset = th.dataOffset;
        dispatch(base, avail, info);
        out.tags.push_back(info);
        pos = th.dataEnd;
    }
}

int Parser::parseFile(const std::string& path, ParseResult& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        out = ParseResult{};
        out.error = "cannot open file: " + path;
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        out = ParseResult{};
        out.error = "empty file";
        return -1;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return parse(buf.data(), got, out);
}

}  // namespace swf
