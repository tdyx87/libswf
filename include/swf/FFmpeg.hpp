#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "swf/Audio.hpp"   // PcmData

// 跨平台音视频解码后端（FFmpeg）。
// 构建时若未定义 SWF_HAVE_FFMPEG，下列函数/类退化为返回 false 的桩，
// 上层据此提示“需要 FFmpeg 后端”（即先 conan install ffmpeg 再重新构建）。
#ifdef SWF_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>       // AVBSFContext / av_bsf_*（AVC: h264_mp4toannexb）
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace swf {

// 把压缩音频（MP3 / Nellymoser / Speex / AAC 等）的原始字节解码为 16-bit PCM。
// format: SWF SoundFormat 编码（2=MP3, 4/5/6=Nellymoser, 7/11=Speex, 10=AAC）。
// 返回 false 时 err 给出原因。
bool ffmpegDecodeAudio(std::uint8_t format, std::uint8_t rate, std::uint8_t bits,
                       std::uint8_t channels, const std::vector<std::uint8_t>& data,
                       PcmData& out, std::string& err);

// 视频解码器：每个 VideoStream 持有一个实例。
// SWF 视频帧（H.263/VP6/AVC）帧间有依赖（P/B 帧），解码器必须跨帧保持状态。
class FFmpegVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder();
    FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    // 打开解码器。codecId: 2=H.263(FLV1), 4=VP6, 5=VP6+alpha, 7=AVC(H.264)。
    // extradata: AVC 的 AVCDecoderConfigurationRecord（可为空；非 AVC 忽略）。
    bool open(std::uint8_t codecId, std::uint16_t width, std::uint16_t height,
              const std::vector<std::uint8_t>& extradata, std::string& err);

    // 解码一帧压缩数据 -> RGBA。成功时 rgbaOut 非空（width*height*4）；失败返回 false。
    bool decodeFrame(const std::vector<std::uint8_t>& frame,
                     std::vector<std::uint8_t>& rgbaOut, int& outW, int& outH);

    const std::string& lastError() const { return err_; }

private:
#ifdef SWF_HAVE_FFMPEG
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVBSFContext* bsf_ = nullptr;   // AVC: h264_mp4toannexb
    bool opened_ = false;
#endif
    std::string err_;
};

}  // namespace swf
