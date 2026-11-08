// 音视频解码测试（CTest: swfparser_audio）。
// 确定性用例：未压缩 PCM(0/3) 与 SWF ADPCM(1) 解码为 16-bit PCM 并核对样本；
// 压缩编码（MP3/Nellymoser/Speex 等）经 FFmpeg 跨平台解码，若构建已启用则校验。
#include "swf/Decoder.hpp"
#include "swf/Audio.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool c, const char* w) {
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w);
    if (!c) ++g_fail;
}

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    int bitBuf = 0, bitCnt = 0;
    void writeUB(std::uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bitBuf = (bitBuf << 1) | static_cast<int>((v >> i) & 1u);
            if (++bitCnt == 8) { bytes.push_back(static_cast<std::uint8_t>(bitBuf)); bitBuf = 0; bitCnt = 0; }
        }
    }
    void align() { if (bitCnt > 0) { bitBuf <<= (8 - bitCnt); bytes.push_back(static_cast<std::uint8_t>(bitBuf)); bitBuf = 0; bitCnt = 0; } }
    void u8(std::uint8_t v) { align(); bytes.push_back(v); }
    void u16(std::uint16_t v) { align(); bytes.push_back(v & 0xFF); bytes.push_back((v >> 8) & 0xFF); }
    void rect(int x0, int y0, int x1, int y1) {
        int m = std::max(std::max(std::abs(x0), std::abs(x1)), std::max(std::abs(y0), std::abs(y1)));
        int nb = 1; while ((1 << (nb - 1)) - 1 < m && nb < 30) ++nb;
        writeUB(static_cast<std::uint32_t>(nb), 5);
        writeUB(static_cast<std::uint32_t>(x0), nb); writeUB(static_cast<std::uint32_t>(x1), nb);
        writeUB(static_cast<std::uint32_t>(y0), nb); writeUB(static_cast<std::uint32_t>(y1), nb);
        align();
    }
};
static void writeTag(std::vector<std::uint8_t>& out, int code, const std::vector<std::uint8_t>& data) {
    std::size_t len = data.size();
    std::uint16_t h = static_cast<std::uint16_t>((code << 6) | (len < 63 ? (len & 0x3F) : 0x3F));
    out.push_back(h & 0xFF); out.push_back((h >> 8) & 0xFF);
    if (len >= 63) for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
    for (auto b : data) out.push_back(b);
}

// 构造含单个 DefineSound 的 SWF 并解码出该声音
static swf::PcmData buildAndDecode(std::uint8_t format, std::uint8_t rate, std::uint8_t bits, std::uint8_t ch,
                                   const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> ds;
    ds.push_back(1); ds.push_back(0);                          // id=1
    ds.push_back(static_cast<std::uint8_t>(((format & 0x0F) << 4) | ((rate & 0x03) << 2) | ((bits & 1) << 1) | (ch & 1)));
    for (int i = 0; i < 4; ++i) ds.push_back(0);               // sampleCount=0
    for (auto b : data) ds.push_back(b);

    std::vector<std::uint8_t> body;
    { BitWriter hdr; hdr.rect(0, 0, 6400, 4800); for (auto b : hdr.bytes) body.push_back(b); }
    body.push_back(0x00); body.push_back(0x18);
    body.push_back(0x01); body.push_back(0x00);
    writeTag(body, 14, ds);
    writeTag(body, 0, {});

    std::vector<std::uint8_t> swf;
    swf.push_back('F'); swf.push_back('W'); swf.push_back('S'); swf.push_back(8);
    std::uint32_t total = 8 + static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) swf.push_back(static_cast<std::uint8_t>((total >> (8 * i)) & 0xFF));
    for (auto b : body) swf.push_back(b);

    swf::Movie movie; std::string err;
    swf::PcmData pcm;
    pcm.samples.clear();
    if (!swf::decodeMovie(swf.data(), swf.size(), movie, err)) { std::printf("  decodeMovie failed: %s\n", err.c_str()); return pcm; }
    auto it = movie.sounds.find(1);
    if (it == movie.sounds.end()) { std::printf("  sound not found\n"); return pcm; }
    if (!swf::decodeSound(it->second, pcm, err)) { std::printf("  decodeSound failed: %s\n", err.c_str()); pcm.samples.clear(); }
    return pcm;
}

int main() {
    // 1) PCM 8-bit 无符号 (format 0, bits=0)：{0,128,255} -> {-32768, 0, 32512}
    {
        std::vector<std::uint8_t> d = {0, 128, 255};
        auto p = buildAndDecode(0, 1, 0, 0, d);
        check(p.samples.size() == 3, "PCM8: 3 samples");
        if (p.samples.size() == 3) {
            check(p.samples[0] == -32768 && p.samples[1] == 0 && p.samples[2] == 32512, "PCM8: {0,128,255}->{-32768,0,32512}");
        }
    }
    // 2) PCM 16-bit 小端 (format 3, bits=1)：{1,2,-3}
    {
        std::vector<std::uint8_t> d = {0x01,0x00, 0x02,0x00, 0xFD,0xFF};
        auto p = buildAndDecode(3, 1, 1, 0, d);
        check(p.samples.size() == 3, "PCM16LE: 3 samples");
        if (p.samples.size() == 3)
            check(p.samples[0] == 1 && p.samples[1] == 2 && p.samples[2] == -3, "PCM16LE: {1,2,-3}");
    }
    // 3) SWF ADPCM (format 1)：header predictor=0 + 半字节 0b0001 -> 样本 [0, 1, ...]
    {
        // 22-bit header (pred=0, idx=0) 随后 nibble=0b0001
        std::vector<std::uint8_t> d = {0x00, 0x00, 0x00, 0x40};
        auto p = buildAndDecode(1, 1, 1, 0, d);
        check(p.samples.size() >= 2, "ADPCM: decoded >=2 samples");
        if (p.samples.size() >= 2)
            check(p.samples[0] == 0 && p.samples[1] == 1, "ADPCM: header=0 then nibble#1 -> +1");
    }
    // 4) MP3 (format 2)：样本来自 SWF_TEST_FILE 或相对路径（都不存在则跳过）
    {
        std::vector<std::string> candidates = {"swf.swf", "../swf.swf", "../../swf.swf"};
        if (const char* env = std::getenv("SWF_TEST_FILE"))
            candidates.insert(candidates.begin(), env);
        FILE* f = nullptr;
        for (const auto& c : candidates) {
            f = std::fopen(c.c_str(), "rb");
            if (f) break;
        }
        if (!f) {
            std::printf("  [SKIP] MP3: no sample (set SWF_TEST_FILE to enable)\n");
        } else {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n <= 0) {
                std::printf("  [SKIP] MP3: empty sample\n");
                std::fclose(f);
            } else {
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
                const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
                std::fclose(f);
                buf.resize(got);
                swf::Movie movie;
                std::string err;
                if (!swf::decodeMovie(buf.data(), buf.size(), movie, err)) {
                    // 找到样本却解析失败是真失败，不能再用 SKIP 掩盖。
                    check(false, "MP3: sample decode failed");
                } else {
                    auto it = movie.sounds.find(47);
                    if (it != movie.sounds.end()) {
                        swf::PcmData pcm;
                        bool ok = swf::decodeSound(it->second, pcm, err);
                        if (!ok && err.find("FFmpeg") != std::string::npos) {
                            // 压缩编码需 FFmpeg 后端；未启用时跳过（跨平台最小构建）
                            std::printf("  [SKIP] MP3: FFmpeg backend unavailable (%s)\n", err.c_str());
                        } else {
                            check(ok && !pcm.samples.empty(), "MP3(id47): decoded non-empty PCM");
                            if (ok) check(pcm.rate == 11025 && pcm.channels == 1, "MP3(id47): 11025Hz mono");
                        }
                    } else {
                        std::printf("  [SKIP] MP3: sound id 47 not found\n");
                    }
                }
            }
        }
    }

    std::printf("\nAudio decode test: %s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
