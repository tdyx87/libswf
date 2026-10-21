#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "swf/Decoder.hpp"

namespace swf {

// 解码后的 16-bit 整型 PCM（通道交织）
struct PcmData {
    int rate = 44100;       // Hz
    int channels = 1;       // 1 单声道 / 2 立体声
    int bits = 16;          // 恒为 16
    std::vector<std::int16_t> samples;   // [L,R,L,R,...]
};

// 将 DefineSound 解码为 PCM。支持 format：
//   0 / 3  未压缩 PCM（分别平台序 / 小端）—— 纯 C++，零依赖，跨平台
//   1      ADPCM（SWF IMA 变体）—— 纯 C++，零依赖，跨平台
//   2      MP3 —— 经 FFmpeg（跨平台；构建需 conan install ffmpeg）
//   4/5/6  Nellymoser、7/11 Speex、10 AAC —— 经 FFmpeg（跨平台）
// 未启用 FFmpeg（SWF_HAVE_FFMPEG 未定义）时，压缩格式返回 false 并提示需要 FFmpeg。
bool decodeSound(const Sound& s, PcmData& out, std::string& err);

// 将整条流式声音（SoundStreamHead + 各帧 SoundStreamBlock）解码为 PCM。
// 仅支持 format 0/1/3（MP3 流式需外部解码器）。
bool decodeStreamSound(const Movie& m, PcmData& out, std::string& err);

// 写出 16-bit WAV 文件；返回是否成功。
bool writeWav(const std::string& path, const PcmData& pcm);

// Windows 实时播放（waveOut）。非 Windows 返回 false 并说明。
bool playPcm(const PcmData& pcm, std::string& err);

// 实时 PCM 流式输出（waveOut 后台线程 + FIFO 队列）。
// 用于播放器逐帧供给 StartSound / SoundStreamBlock，实现音画同步。
// Windows 上真实播放；其它平台为 no-op（queue 被静默丢弃），保证跨平台可编译。
class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // 打开设备（幂等：已打开且参数一致直接返回 true）。
    bool open(int rate, int channels);
    // 入队交织 16-bit PCM。loop=true 时该段播完自动重入队（直到 stopAll）。
    void queue(const std::int16_t* samples, std::size_t count, bool loop = false);
    // 停止播放并清空队列（含循环段）。
    void stopAll();
    // 关闭设备。
    void close();
    bool isOpen() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// 音视/频编码名称（诊断输出用）
const char* soundFormatName(std::uint8_t fmt);

}  // namespace swf
