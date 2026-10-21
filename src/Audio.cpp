// 音视频解码与渲染后端。
//   - DefineSound / SoundStreamBlock 解码为 16-bit PCM
//   - PCM(0/3)、ADPCM(1)：纯 C++ 自实现（零依赖，跨平台）
//   - MP3(2)、Nellymoser(4/5/6)、Speex(7/11)、AAC(10)：经 FFmpeg（跨平台，见 FFmpeg.cpp）
//   - 输出 WAV（跨平台），或经 waveOut 实时播放（仅 Windows；其余平台用 --wav 导出）
#include "swf/Audio.hpp"
#include "swf/FFmpeg.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
// 仅 playPcm() 用到的 Windows 实时播放 API（waveOut）。FFmpeg 已取代 Media Foundation。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace swf {

static const int kRate[4] = {5512, 11025, 22050, 44100};

const char* soundFormatName(std::uint8_t fmt) {
    switch (fmt) {
        case 0:  return "PCM";
        case 1:  return "ADPCM";
        case 2:  return "MP3";
        case 3:  return "PCM-LE";
        case 4:  return "Nellymoser8k";
        case 5:  return "Nellymoser";
        case 6:  return "Nellymoser16k";
        case 7:  return "Speex";
        case 10: return "AAC";
        default: return "?";
    }
}

// ---------- 未压缩 PCM（format 0 平台序 / 3 小端；x86 上均为 LE）----------
static bool decodePcm(const Sound& s, PcmData& out, bool /*le*/) {
    out.rate = kRate[s.rate & 3];
    out.channels = s.channels ? 2 : 1;
    out.bits = 16;
    const std::uint8_t* p = s.data.data();
    std::size_t n = s.data.size();
    if (s.bits) {                              // 16-bit signed
        std::size_t count = n / 2;
        out.samples.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t lo = p[2 * i], hi = p[2 * i + 1];
            out.samples[i] = static_cast<std::int16_t>(lo | (hi << 8));   // LE（x86）
        }
    } else {                                   // 8-bit unsigned -> 16-bit
        out.samples.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            out.samples[i] = static_cast<std::int16_t>((static_cast<int>(p[i]) - 128) * 256);
    }
    return true;
}

// ---------- SWF ADPCM（IMA 变体，位流：每通道 16-bit predictor + 6-bit index，后接交织 4-bit 半字节）----------
static const int kAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 40, 44,
    49, 54, 59, 65, 72, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
    279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};
static const int kAdpcmIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static bool decodeAdpcm(const Sound& s, PcmData& out) {
    out.rate = kRate[s.rate & 3];
    out.channels = s.channels ? 2 : 1;
    out.bits = 16;
    const std::uint8_t* p = s.data.data();
    std::size_t nbytes = s.data.size();
    if (nbytes < static_cast<std::size_t>(out.channels) * 3) return true;
    long bitpos = 0;
    auto getbits = [&](int nb) -> int {
        int v = 0;
        for (int i = 0; i < nb; ++i) {
            std::size_t bp = bitpos >> 3;
            if (bp >= nbytes) return 0;
            int bit = (p[bp] >> (7 - (bitpos & 7))) & 1;
            v = (v << 1) | bit;
            ++bitpos;
        }
        return v;
    };
    struct St { int predictor = 0; int index = 0; };
    std::vector<St> st(out.channels);
    for (int c = 0; c < out.channels; ++c) {
        int b0 = getbits(8), b1 = getbits(8);
        int pred = static_cast<int>(static_cast<std::int16_t>(b0 | (b1 << 8)));  // LE int16
        int idx = getbits(6) & 0x3F;
        if (idx > 88) idx = 88;
        st[c].predictor = pred; st[c].index = idx;
        out.samples.push_back(static_cast<std::int16_t>(pred));
    }
    while ((bitpos >> 3) < nbytes) {
        std::size_t need = static_cast<std::size_t>(bitpos) + 4ULL * out.channels;
        if (((need + 7) >> 3) > nbytes) break;   // 不足以构成完整一组半字节则停止
        for (int c = 0; c < out.channels; ++c) {
            int nibble = getbits(4);
            int step = kAdpcmStep[st[c].index];
            int diff = step >> 3;
            if (nibble & 4) diff += step;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 8) st[c].predictor -= diff; else st[c].predictor += diff;
            if (st[c].predictor > 32767) st[c].predictor = 32767;
            if (st[c].predictor < -32768) st[c].predictor = -32768;
            st[c].index += kAdpcmIndex[nibble];
            if (st[c].index < 0) st[c].index = 0;
            if (st[c].index > 88) st[c].index = 88;
            out.samples.push_back(static_cast<std::int16_t>(st[c].predictor));
        }
    }
    return true;
}

bool decodeSound(const Sound& s, PcmData& out, std::string& err) {
    out.rate = kRate[s.rate & 3];
    switch (s.format) {
        case 0: return decodePcm(s, out, false);
        case 3: return decodePcm(s, out, true);
        case 1: return decodeAdpcm(s, out);
        // 压缩编码（MP3/Nellymoser/Speex/AAC）：统一经 FFmpeg 跨平台解码
        case 2: case 4: case 5: case 6: case 7: case 10: case 11:
#ifdef SWF_HAVE_FFMPEG
            return ffmpegDecodeAudio(s.format, s.rate, s.bits, s.channels, s.data, out, err);
#else
            err = "压缩音频(" + std::to_string(s.format) +
                  ") 需 FFmpeg 后端（本构建未定义 SWF_HAVE_FFMPEG）";
            return false;
#endif
        default:
            err = "不支持的音频编码格式(" + std::to_string(s.format) + ")";
            return false;
    }
}

bool decodeStreamSound(const Movie& m, PcmData& out, std::string& err) {
    const auto& sa = m.streamAudio;
    if (!sa.have || sa.blocks.empty()) { err = "无流式声音数据"; return false; }
    PcmData acc;
    acc.bits = 16;
    if (sa.format == 2) {                                   // MP3 流式：经 FFmpeg
#ifndef SWF_HAVE_FFMPEG
        err = "流式 MP3 需 FFmpeg 后端（本构建未定义 SWF_HAVE_FFMPEG）";
        return false;
#else
        acc.rate = 0; acc.channels = sa.channels ? 2 : 1;   // 实际采样率/声道由首帧确定
        for (const auto& blk : sa.blocks) {
            PcmData p;
            if (!ffmpegDecodeAudio(2, sa.rate, sa.bits, sa.channels, blk, p, err)) {
                if (p.samples.empty()) continue;            // 容忍单块失败
            }
            if (!p.samples.empty()) {
                if (acc.rate == 0)     acc.rate = p.rate;
                if (acc.channels == 0) acc.channels = p.channels;
                acc.samples.insert(acc.samples.end(), p.samples.begin(), p.samples.end());
            }
        }
#endif
    } else if (sa.format == 0 || sa.format == 1 || sa.format == 3) {
        acc.rate = kRate[sa.rate & 3];
        acc.channels = sa.channels ? 2 : 1;
        for (const auto& blk : sa.blocks) {
            Sound pseudo;
            pseudo.format = sa.format; pseudo.rate = sa.rate; pseudo.bits = sa.bits;
            pseudo.channels = sa.channels; pseudo.data = blk;
            PcmData p;
            if (sa.format == 1) { if (!decodeAdpcm(pseudo, p)) continue; }
            else                { if (!decodePcm(pseudo, p, sa.format == 3)) continue; }
            acc.samples.insert(acc.samples.end(), p.samples.begin(), p.samples.end());
        }
    } else {
        err = "流式声音编码(" + std::to_string(sa.format) + ")暂不支持";
        return false;
    }
    out = acc;
    if (out.samples.empty()) { err = "流式声音解码为空"; return false; }
    return true;
}

bool writeWav(const std::string& path, const PcmData& pcm) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    int ch = pcm.channels, bits = 16;
    std::uint32_t byteRate = static_cast<std::uint32_t>(pcm.rate) * ch * bits / 8;
    std::uint16_t blockAlign = static_cast<std::uint16_t>(ch * bits / 8);
    std::uint32_t dataSize = static_cast<std::uint32_t>(pcm.samples.size() * 2);
    auto w16 = [&](std::uint16_t v) { std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); };
    auto w32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xFF, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(static_cast<std::uint16_t>(ch));
    w32(static_cast<std::uint32_t>(pcm.rate)); w32(byteRate); w16(blockAlign); w16(static_cast<std::uint16_t>(bits));
    std::fwrite("data", 1, 4, f); w32(dataSize);
    for (auto s : pcm.samples) w16(static_cast<std::uint16_t>(static_cast<std::uint16_t>(s)));
    std::fclose(f);
    return true;
}

bool playPcm(const PcmData& pcm, std::string& err) {
#ifdef _WIN32
    if (pcm.samples.empty()) return true;
    HWAVEOUT h = nullptr;
    WAVEFORMATEX wfx;
    std::memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(pcm.channels);
    wfx.nSamplesPerSec = static_cast<DWORD>(pcm.rate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(pcm.channels * 2);
    wfx.nAvgBytesPerSec = static_cast<DWORD>(pcm.rate) * wfx.nBlockAlign;
    if (waveOutOpen(&h, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        err = "waveOutOpen 失败（无音频设备？）"; return false;
    }
    WAVEHDR hdr;
    std::memset(&hdr, 0, sizeof hdr);
    hdr.lpData = reinterpret_cast<LPSTR>(const_cast<std::int16_t*>(pcm.samples.data()));
    hdr.dwBufferLength = static_cast<DWORD>(pcm.samples.size() * 2);
    waveOutPrepareHeader(h, &hdr, sizeof hdr);
    waveOutWrite(h, &hdr, sizeof hdr);
    while (!(hdr.dwFlags & WHDR_DONE)) Sleep(10);
    waveOutUnprepareHeader(h, &hdr, sizeof hdr);
    waveOutClose(h);
    return true;
#else
    err = "实时播放仅支持 Windows（waveOut）"; return false;
#endif
}

// ---------- AudioOut：waveOut 流式输出（后台线程 + FIFO） ----------
struct AudioOut::Impl {
    bool open_ = false;
    int rate_ = 44100, channels_ = 2;
#ifdef _WIN32
    HWAVEOUT h_ = nullptr;
    static constexpr int kBufCount = 3;
    static constexpr int kBufSamples = 1024;          // 每缓冲采样帧（11kHz 下约 93ms）
    WAVEHDR hdrs_[kBufCount]{};
    std::vector<std::int16_t> bufs_[kBufCount];
    std::thread worker_;
#endif
    std::mutex mtx_;
    std::condition_variable cv_;
    bool quit_ = false;
    // 活跃音效：Flash 可同时播放多个声音（多个声道），这里把各段**混音**进输出缓冲，
    // 而不是排队依次播放（串行会让后触发的音效被前一个长音效整体推迟，表现为音画不同步）。
    struct Seg {
        std::vector<std::int16_t> data;
        std::size_t pos = 0;
        bool loop = false;
    };
    std::deque<Seg> queue_;
};

AudioOut::AudioOut() : impl_(new Impl) {}
AudioOut::~AudioOut() { close(); delete impl_; }

bool AudioOut::open(int rate, int channels) {
    std::lock_guard<std::mutex> lk(impl_->mtx_);
    if (impl_->open_ && impl_->rate_ == rate && impl_->channels_ == channels) return true;
#ifdef _WIN32
    // 已打开但参数变化：先关闭底层设备（不重复加锁）
    if (impl_->h_) {
        waveOutReset(impl_->h_);
        for (auto& hdr : impl_->hdrs_)
            if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(impl_->h_, &hdr, sizeof hdr);
        waveOutClose(impl_->h_);
        impl_->h_ = nullptr;
    }
    impl_->quit_ = false;
    WAVEFORMATEX wfx;
    std::memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = static_cast<DWORD>(rate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(channels * 2);
    wfx.nAvgBytesPerSec = static_cast<DWORD>(rate) * wfx.nBlockAlign;
    if (waveOutOpen(&impl_->h_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        impl_->h_ = nullptr;
        return false;
    }
    for (int i = 0; i < Impl::kBufCount; ++i) {
        impl_->bufs_[i].assign(static_cast<std::size_t>(Impl::kBufSamples) * channels, 0);
        WAVEHDR& hdr = impl_->hdrs_[i];
        std::memset(&hdr, 0, sizeof hdr);
        hdr.lpData = reinterpret_cast<LPSTR>(impl_->bufs_[i].data());
        hdr.dwBufferLength = static_cast<DWORD>(impl_->bufs_[i].size() * sizeof(std::int16_t));
        waveOutPrepareHeader(impl_->h_, &hdr, sizeof hdr);
    }
    impl_->open_ = true;
    impl_->rate_ = rate;
    impl_->channels_ = channels;
    // 启动后台线程：从 FIFO 填充波形缓冲并提交给 waveOut
    Impl* imp = impl_;
    imp->worker_ = std::thread([imp]() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(imp->mtx_);
                imp->cv_.wait_for(lk, std::chrono::milliseconds(10),
                                  [imp] { return imp->quit_; });
                if (imp->quit_) break;
            }
            if (!imp->h_) continue;
            for (int i = 0; i < Impl::kBufCount; ++i) {
                WAVEHDR& hdr = imp->hdrs_[i];
                if (hdr.dwFlags & WHDR_INQUEUE) continue;
                if (hdr.dwFlags & WHDR_DONE) {
                    waveOutUnprepareHeader(imp->h_, &hdr, sizeof hdr);
                    std::memset(&hdr, 0, sizeof hdr);
                    hdr.lpData = reinterpret_cast<LPSTR>(imp->bufs_[i].data());
                    hdr.dwBufferLength = static_cast<DWORD>(imp->bufs_[i].size() * sizeof(std::int16_t));
                    waveOutPrepareHeader(imp->h_, &hdr, sizeof hdr);
                }
                if (hdr.dwFlags & WHDR_PREPARED && !(hdr.dwFlags & WHDR_INQUEUE)) {
                    // 把所有活跃音效混音进本缓冲（加法混合 + 饱和限幅）。
                    auto& buf = imp->bufs_[i];
                    const std::size_t need = buf.size();      // int16 个数
                    std::size_t maxWritten = 0;
                    {
                        std::lock_guard<std::mutex> lk(imp->mtx_);
                        std::memset(buf.data(), 0, need * sizeof(std::int16_t));
                        for (std::size_t s = 0; s < imp->queue_.size(); ) {
                            auto& seg = imp->queue_[s];
                            if (seg.data.empty()) {
                                imp->queue_.erase(imp->queue_.begin() + static_cast<std::ptrdiff_t>(s));
                                continue;
                            }
                            std::size_t written = 0;
                            bool finished = false;
                            while (written < need) {
                                const std::size_t avail = seg.data.size() - seg.pos;
                                const std::size_t take = std::min(need - written, avail);
                                for (std::size_t k = 0; k < take; ++k) {
                                    const int v = static_cast<int>(buf[written + k]) +
                                                  static_cast<int>(seg.data[seg.pos + k]);
                                    buf[written + k] = static_cast<std::int16_t>(
                                        v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                                }
                                written += take;
                                seg.pos += take;
                                if (seg.pos >= seg.data.size()) {
                                    if (seg.loop) seg.pos = 0;
                                    else { finished = true; break; }
                                }
                                if (take == 0) break;
                            }
                            if (written > maxWritten) maxWritten = written;
                            if (finished) imp->queue_.erase(imp->queue_.begin() + static_cast<std::ptrdiff_t>(s));
                            else ++s;
                        }
                    }
                    if (maxWritten == 0) continue;          // 无活跃音效：不提交空缓冲
                    hdr.dwBufferLength = static_cast<DWORD>(maxWritten * sizeof(std::int16_t));
                    waveOutWrite(imp->h_, &hdr, sizeof hdr);
                }
            }
        }
    });
    return true;
#else
    (void)rate; (void)channels;
    impl_->open_ = false;   // 非 Windows：no-op 模式
    return true;
#endif
}

void AudioOut::queue(const std::int16_t* samples, std::size_t count, bool loop) {
    if (!samples || count == 0) return;
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lk(impl_->mtx_);
        Impl::Seg seg;
        seg.data.assign(samples, samples + count);
        seg.loop = loop;
        impl_->queue_.push_back(std::move(seg));
    }
    impl_->cv_.notify_one();
#else
    (void)loop;
#endif
}

void AudioOut::stopAll() {
    std::lock_guard<std::mutex> lk(impl_->mtx_);
    impl_->queue_.clear();
#ifdef _WIN32
    if (impl_->h_) waveOutReset(impl_->h_);
#endif
}

void AudioOut::close() {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx_);
        impl_->quit_ = true;
        impl_->queue_.clear();
    }
    impl_->cv_.notify_all();
#ifdef _WIN32
    if (impl_->worker_.joinable()) impl_->worker_.join();
    if (impl_->h_) {
        waveOutReset(impl_->h_);
        for (auto& hdr : impl_->hdrs_)
            if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(impl_->h_, &hdr, sizeof hdr);
        waveOutClose(impl_->h_);
        impl_->h_ = nullptr;
    }
#endif
    impl_->open_ = false;
}

bool AudioOut::isOpen() const { return impl_->open_; }

}  // namespace swf
