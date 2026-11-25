#pragma once
#include <cstdint>
#include <memory>
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

// 以线性插值把 PCM 重采样到 targetRate（原地修改，保留声道交织顺序）。
// 混音设备只按单一采样率工作，不同采样率的声音必须转换后再入队。
void resamplePcm(PcmData& pcm, int targetRate);

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
    // 入队交织 16-bit PCM。repeats 是**额外重复次数**（AS3 Sound.play 的 loops
    // 语义）：0 只播一遍，1 播两遍……负数表示无限循环（直到 stopAll/stopChannel）。
    // 返回该段的声道句柄（>0，供 stopChannel 使用）；设备不可用时返回 0。
    int queue(const std::int16_t* samples, std::size_t count, int repeats = 0);
    // 设置声道音量（0..1），对应 AS3 SoundTransform.volume。
    // swf2 的 MusicButton 通过给 SoundChannel 赋 SoundTransform 实现静音/恢复。
    void setChannelVolume(int id, float volume);
    // 停止并移除指定声道的片段（AS3 SoundChannel.stop / Sound.close）。
    void stopChannel(int id);
    // 停止播放并清空队列（含循环段）。
    void stopAll();
    // 关闭设备。
    void close();
    bool isOpen() const;
    // 当前设备采样率（未打开时为 0）。
    int rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 音视/频编码名称（诊断输出用）
const char* soundFormatName(std::uint8_t fmt);

}  // namespace swf
