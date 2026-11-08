#include "swf/Parser.hpp"

#include <cstdio>
#include <cstring>

#include "swf/BitReader.hpp"
#include "swf/Decompress.hpp"
#include "swf/TagReader.hpp"

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
    // 唯一实现放在 swf::readTagHeader，Decoder 复用同一份，避免两套分叉。
    return swf::readTagHeader(data, pos, avail, th) == TagStatus::Ok ? 0 : -1;
}

// sub_48BA50 的标签分派（此处做浅解析，仅还原逆向确认的字段）
void Parser::dispatch(const std::uint8_t* data, std::size_t avail, TagInfo& info) {
    const std::size_t off = info.dataOffset;
    const std::uint32_t len = info.length;
    if (off > avail || len > avail - off) return;

    // Strategy 化的浅解析同样只拿 tag 边界视图，越界读取自动返回 0 并留痕。
    TagReader r(data + off, len, info.code);

    switch (info.code) {
        case 9: {  // SetBackgroundColor：sub_47A7A0 读 RGB
            auto c = r.readRGBA(false);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "RGB(#%02X%02X%02X)", c.r, c.g, c.b);
            info.note = buf;
            break;
        }
        case 2: case 22: case 32: case 83: {  // DefineShape*
            info.shapeId = r.readUI16();
            BitReader br(r.bytes());
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
            info.shapeId = r.readUI16();
            info.note = "free id=" + std::to_string(info.shapeId);
            break;
        }
        case 69: {  // FileAttributes
            std::uint32_t flags = r.readUI32();
            info.note = "flags=0x" + std::to_string(flags);
            break;
        }
        case 65: {  // ScriptLimits
            std::uint16_t depth = r.readUI16();
            std::uint16_t timeout = r.readUI16();
            info.note = "depth=" + std::to_string(depth) +
                        " timeout=" + std::to_string(timeout) + "s";
            break;
        }
        case 76: {  // SymbolClass：UI16 count + (UI16 tag, 名称) 序列
            std::uint16_t count = r.readUI16();
            info.note = "symbols=" + std::to_string(count);
            break;
        }
        case 26: {  // PlaceObject2：UI8 flags + UI16 depth + [UI16 cid] + [matrix] + ... + [name]
            // 浅解析只取 flags/depth/cid/name（SWF 规范 §13.3）；矩阵按位跳过，色变/比率不展开。
            std::uint8_t flags = r.readUI8();
            std::uint16_t depth = r.readUI16();
            std::uint16_t cid = 0;
            std::string nm;
            if (flags & 0x02) cid = r.readUI16();
            if (flags & 0x04) {                     // MATRIX：位域跳过
                BitReader br(r.bytes());
                if (br.readUB(1)) {                 // HasScale
                    int nb = static_cast<int>(br.readUB(5));
                    (void)br.readSB(nb); (void)br.readSB(nb);
                }
                if (br.readUB(1)) {                 // HasRotate
                    int nb = static_cast<int>(br.readUB(5));
                    (void)br.readSB(nb); (void)br.readSB(nb);
                }
                int nb = static_cast<int>(br.readUB(5));
                (void)br.readSB(nb); (void)br.readSB(nb);
                br.finish();
            }
            if (flags & 0x08) {                     // CXFORMWITHALPHA：bit 估算长度，直接按
                BitReader br(r.bytes());            // 规范位域跳过
                int hasAdd = static_cast<int>(br.readUB(1));
                int hasMult = static_cast<int>(br.readUB(1));
                int nb = static_cast<int>(br.readUB(4));
                int terms = hasMult ? 4 : 0;
                for (int k = 0; k < terms; ++k) (void)br.readSB(nb);
                for (int k = 0; k < (hasAdd ? 4 : 0); ++k) (void)br.readSB(nb);
                br.finish();
            }
            if (flags & 0x10) (void)r.readUI16();   // ratio
            if (flags & 0x20) {                     // name：NUL 结尾字符串
                while (r.has(1)) {
                    char ch = static_cast<char>(r.readUI8());
                    if (ch == '\0') break;
                    nm.push_back(ch);
                }
            }
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s depth=%u char=%u%s%s flags=0x%02X",
                          (flags & 0x01) ? "move" : "place", depth, cid,
                          nm.empty() ? "" : " name=\"", nm.c_str(), flags);
            info.note = buf;
            break;
        }
        case 28: {  // RemoveObject2：UI16 depth
            std::uint16_t depth = r.readUI16();
            info.note = "remove depth=" + std::to_string(depth);
            break;
        }
        case 39: {  // DefineSprite：UI16 id + UI16 frameCount + 控制标签序列
            std::uint16_t id = r.readUI16();
            std::uint16_t nf = r.readUI16();
            info.shapeId = id;
            info.note = "id=" + std::to_string(id) + " frames=" + std::to_string(nf);
            break;
        }
        case 82: {  // DoABC：逆向中仅登记位置，解码推迟到 ShowFrame 前
            info.note = "abc block (" + std::to_string(len) + " bytes, deferred)";
            break;
        }
        case 93: {  // EnableTelemetry：UI16 保留 + 32 字节口令/哈希
            r.readUI16();
            info.note = "telemetry password hash (" + std::to_string(len) + " bytes)";
            break;
        }
        case 43: {  // FrameLabel
            info.note = "frame label at " + std::to_string(off);
            break;
        }
        case 60: {  // DefineVideoStream：UI16 id + UI16 numFrames + UI16 W + UI16 H + UI8 VideoFlags
            std::uint16_t id = r.readUI16();
            std::uint16_t nf = r.readUI16();
            std::uint16_t w = r.readUI16();
            std::uint16_t h = r.readUI16();
            std::uint8_t vf = r.readUI8();
            static const char* kCodec[] = {"undefined", "reserved", "H.263", "ScreenVideo",
                                           "VP6", "VP6+alpha", "ScreenVideo2", "AVC"};
            std::uint8_t cid = vf & 0x0F;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "id=%u %dx%d frames=%u codec=%s%s%s",
                          id, w, h, nf, cid < 8 ? kCodec[cid] : "?",
                          (vf & 0x10) ? " smoothing" : "", (vf & 0x20) ? " deblocking" : "");
            info.note = buf;
            break;
        }
        case 61: {  // VideoFrame：UI16 StreamID + UI16 FrameNum + 压缩数据
            std::uint16_t sid = r.readUI16();
            std::uint16_t fnum = r.readUI16();
            info.note = "stream=" + std::to_string(sid) + " frame=" + std::to_string(fnum) +
                        " (" + std::to_string(len >= 4 ? len - 4 : 0) + " bytes)";
            break;
        }
        case 14: {  // DefineSound：UI16 SoundID + UI8 flags（格式/采样率/位深/声道）
            std::uint16_t id = r.readUI16();
            std::uint8_t flags = r.readUI8();
            static const char* kFmt[] = {"Uncompressed", "ADPCM", "MP3", "Nellymoser",
                                         "PCM-LE", "Speex", "reserved6", "MP3-8",
                                         "reserved8", "Nellymoser-16"};
            // 高 4 位可达 15，而表只有 10 项；越界时必须回落到 "unknown"。
            const std::uint8_t fi = (flags >> 4) & 0x0Fu;
            const char* fmtName = fi < sizeof(kFmt) / sizeof(kFmt[0]) ? kFmt[fi] : "unknown";
            // SWF 采样率表不是 5512<<n（11024≠11025），必须用精确值
            static const int kRate[4] = {5512, 11025, 22050, 44100};
            char buf[96];
            std::snprintf(buf, sizeof(buf), "id=%u %s %dHz %dbit %dch",
                          id, fmtName, kRate[(flags >> 2) & 3],
                          ((flags >> 1) & 1) ? 16 : 8, (flags & 1) ? 2 : 1);
            info.note = buf;
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
            ok = decodeZws(data + 8, size - 8, plain);
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
        if (pos > avail || avail - pos < 2)
            return out.tags.empty() ? -1 : 1;  // 1 = 数据不足待续
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
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        out = ParseResult{};
        out.error = "seek failed: " + path;
        return -1;
    }
    long n = std::ftell(f);
    if (n <= 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        out = ParseResult{};
        out.error = "empty file or seek failed: " + path;
        return -1;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        out = ParseResult{};
        out.error = "short read: " + path;
        return -1;
    }
    return parse(buf.data(), got, out);
}

}  // namespace swf
