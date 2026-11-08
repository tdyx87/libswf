// 跨平台音视频解码后端（FFmpeg）。
//   - 音频：MP3 / Nellymoser / Speex / AAC 等 -> 16-bit PCM（libavcodec + libswresample）
//   - 视频：H.263(FLV1) / VP6 / VP6+alpha / AVC(H.264) -> RGBA（libavcodec + libswscale）
// 未启用 SWF_HAVE_FFMPEG 时退化为返回 false 的桩，便于无 FFmpeg 的环境仍能编译。
#include "swf/FFmpeg.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace swf {

#ifdef SWF_HAVE_FFMPEG

// RAII：FFmpeg 裸指针在作用域结束时自动释放（替代手工 av_*_free，消除提前返回/异常路径的泄漏）。
static inline auto raiiCtx(AVCodecContext* p) {
    return std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>(p, [](AVCodecContext* x){ if (x) avcodec_free_context(&x); });
}
static inline auto raiiPacket(AVPacket* p) {
    return std::unique_ptr<AVPacket, void(*)(AVPacket*)>(p, [](AVPacket* x){ if (x) av_packet_free(&x); });
}
static inline auto raiiFrame(AVFrame* p) {
    return std::unique_ptr<AVFrame, void(*)(AVFrame*)>(p, [](AVFrame* x){ if (x) av_frame_free(&x); });
}
static inline auto raiiParser(AVCodecParserContext* p) {
    return std::unique_ptr<AVCodecParserContext, void(*)(AVCodecParserContext*)>(p, av_parser_close);
}
static inline auto raiiSws(SwsContext* p) {
    return std::unique_ptr<SwsContext, void(*)(SwsContext*)>(p, sws_freeContext);
}

static const int kSR[4] = {5512, 11025, 22050, 44100};

static enum AVCodecID swfAudioCodecId(std::uint8_t f) {
    switch (f) {
        case 2:  return AV_CODEC_ID_MP3;
        case 4:  case 5: case 6: return AV_CODEC_ID_NELLYMOSER;
        case 7:  case 11: return AV_CODEC_ID_SPEEX;
        case 10: return AV_CODEC_ID_AAC;
        default: return AV_CODEC_ID_NONE;
    }
}

// ---------- 音频：压缩音频 -> 16-bit PCM ----------
bool ffmpegDecodeAudio(std::uint8_t format, std::uint8_t rate, std::uint8_t bits,
                       std::uint8_t channels, const std::vector<std::uint8_t>& data,
                       PcmData& out, std::string& err) {
    (void)bits;
    enum AVCodecID cid = swfAudioCodecId(format);
    if (cid == AV_CODEC_ID_NONE) {
        err = "FFmpeg: 不支持的音频格式(" + std::to_string(format) + ")";
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(cid);
    if (!codec) {
        err = "FFmpeg: 未找到对应解码器（该 FFmpeg 构建可能未启用此编码）";
        return false;
    }
    auto ctxOwner = raiiCtx(avcodec_alloc_context3(codec));
    AVCodecContext* ctx = ctxOwner.get();
    if (!ctx) { err = "FFmpeg: avcodec_alloc_context3 失败"; return false; }
    ctx->sample_rate = kSR[rate & 3];                          // 提示；多数编码会从码流推断
    av_channel_layout_default(&ctx->ch_layout, channels ? 2 : 1);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        err = "FFmpeg: avcodec_open2 失败";   // ctxOwner 析构自动释放
        return false;
    }

    auto pktOwner = raiiPacket(av_packet_alloc());
    auto frameOwner = raiiFrame(av_frame_alloc());
    auto parserOwner = raiiParser(av_parser_init(cid));
    AVPacket* pkt = pktOwner.get();
    AVFrame* frame = frameOwner.get();
    AVCodecParserContext* parser = parserOwner.get();
    if (!pkt || !frame) {
        err = "FFmpeg: 分配 AVPacket/AVFrame 失败";   // 四个 owner 析构自动释放
        return false;
    }
    // swresample 上下文在第一帧才创建，交给 owner 接管（提前返回/异常自动 swr_free）。
    std::unique_ptr<SwrContext, void(*)(SwrContext*)> swrOwner(
        nullptr, [](SwrContext* x) { if (x) swr_free(&x); });
    SwrContext* swr = nullptr;
    std::vector<std::int16_t> pcm;
    bool ok = true;

    auto convertFrame = [&](AVFrame* f) -> bool {
        if (!swr) {
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, f->ch_layout.nb_channels);
            SwrContext* raw = nullptr;
            int r = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_S16, f->sample_rate,
                                        &f->ch_layout, (AVSampleFormat)f->format, f->sample_rate,
                                        0, nullptr);
            swr = raw;
            swrOwner.reset(raw);   // 先接管，初始化失败也自动释放
            if (r < 0 || swr_init(swr) < 0) {
                err = "FFmpeg: swresample 初始化失败";
                return false;
            }
        }
        if (pcm.empty()) {            // 首帧：记录实际采样率/声道数
            out.rate = f->sample_rate;
            out.channels = f->ch_layout.nb_channels;
            out.bits = 16;
        }
        int dstNb = static_cast<int>(av_rescale_rnd(f->nb_samples, f->sample_rate,
                                                    f->sample_rate, AV_ROUND_UP));
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(dstNb) *
                                      f->ch_layout.nb_channels * 2);
        std::uint8_t* outPtr = buf.data();
        int got = swr_convert(swr, &outPtr, dstNb,
                              (const std::uint8_t**)f->data, f->nb_samples);
        if (got < 0) return false;
        std::size_t nbytes = static_cast<std::size_t>(got) * f->ch_layout.nb_channels * 2;
        std::size_t old = pcm.size();
        pcm.resize(old + nbytes / 2);
        std::memcpy(&pcm[old], buf.data(), nbytes);
        return true;
    };

    if (parser) {
        const std::uint8_t* in = data.data();
        std::size_t off = 0;
        std::size_t guard = 0;
        while (off < data.size()) {
            // av_parser_parse2 可能返回 used=0 / outLen=0，旧实现既不推进 off
            // 也没有退出条件，畸形码流会让线程 100% 空转。
            if (++guard > data.size() + 1024) {
                ok = false; err = "FFmpeg: 解析器未推进"; break;
            }
            std::uint8_t* outBuf = nullptr; int outLen = 0;
            int used = av_parser_parse2(parser, ctx, &outBuf, &outLen,
                                        in + off, static_cast<int>(data.size() - off),
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) { ok = false; err = "FFmpeg: 解析音频流失败"; break; }
            off += static_cast<std::size_t>(used);
            if (used == 0 && outLen == 0) break;
            if (outLen > 0) {
                pkt->data = outBuf; pkt->size = outLen;
                if (avcodec_send_packet(ctx, pkt) == 0) {
                    while (avcodec_receive_frame(ctx, frame) >= 0) {
                        if (!convertFrame(frame)) { ok = false; break; }
                    }
                }
                if (!ok) break;
            }
        }
        // 冲刷解析器尾部（同样加次数上限，避免尾部反复返回同一包）
        std::size_t flushGuard = 0;
        while (ok && ++flushGuard <= 1024) {
            std::uint8_t* outBuf = nullptr; int outLen = 0;
            int used = av_parser_parse2(parser, ctx, &outBuf, &outLen,
                                        nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0 || outLen == 0) break;
            pkt->data = outBuf; pkt->size = outLen;
            if (avcodec_send_packet(ctx, pkt) == 0)
                while (avcodec_receive_frame(ctx, frame) >= 0) convertFrame(frame);
        }
    } else {
        // 无解析器（Nellymoser/Speex 等）：整段作为一包送入
        pkt->data = const_cast<std::uint8_t*>(data.data());
        pkt->size = static_cast<int>(data.size());
        if (data.empty() || avcodec_send_packet(ctx, pkt) < 0) {
            ok = false; err = "FFmpeg: 送入音频包失败";
        } else {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                if (!convertFrame(frame)) { ok = false; break; }
            }
        }
    }
    // 冲刷解码器
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) >= 0) convertFrame(frame);

    // ctxOwner / pktOwner / frameOwner / parserOwner / swrOwner 在作用域结束时自动释放，
    // 覆盖所有提前返回与异常路径（原先的手工 av_*_free / swr_free 已全部移除）。

    if (pcm.empty()) {
        if (ok) err = "FFmpeg: 未解码出任何音频样本";
        return false;
    }
    out.samples = std::move(pcm);
    return true;
}

// ---------- 视频：压缩视频帧 -> RGBA ----------
FFmpegVideoDecoder::FFmpegVideoDecoder() = default;

FFmpegVideoDecoder::~FFmpegVideoDecoder() {
    if (bsf_) av_bsf_free(&bsf_);
    if (ctx_) avcodec_free_context(&ctx_);
}

bool FFmpegVideoDecoder::open(std::uint8_t codecId, std::uint16_t width, std::uint16_t height,
                              const std::vector<std::uint8_t>& extradata, std::string& err) {
    enum AVCodecID cid;
    switch (codecId) {
        case 2: cid = AV_CODEC_ID_FLV1;  break;   // H.263
        case 4: cid = AV_CODEC_ID_VP6;   break;
        case 5: cid = AV_CODEC_ID_VP6A;  break;   // VP6 with alpha
        case 7: cid = AV_CODEC_ID_H264;  break;   // AVC
        default:
            err = "FFmpeg: 不支持的视频编码(" + std::to_string(codecId) + ")";
            return false;
    }
    const AVCodec* codec = avcodec_find_decoder(cid);
    if (!codec) { err = "FFmpeg: 未找到视频解码器"; return false; }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) { err = "FFmpeg: 分配上下文失败"; return false; }
    if (width && height) { ctx_->width = width; ctx_->height = height; }
    if (!extradata.empty()) {
        ctx_->extradata = (std::uint8_t*)av_malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx_->extradata) { err = "FFmpeg: 分配 extradata 失败"; return false; }
        std::memcpy(ctx_->extradata, extradata.data(), extradata.size());
        std::memset(ctx_->extradata + extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        ctx_->extradata_size = static_cast<int>(extradata.size());
    }
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        err = "FFmpeg: 打开视频解码器失败";
        return false;
    }
    // AVC：长度前缀 NAL -> Annex-B（h264_mp4toannexb）
    if (cid == AV_CODEC_ID_H264) {
        const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
        if (f) {
            if (av_bsf_alloc(f, &bsf_) == 0) {
                avcodec_parameters_from_context(bsf_->par_in, ctx_);
                if (av_bsf_init(bsf_) < 0) { av_bsf_free(&bsf_); bsf_ = nullptr; }
            }
        }
    }
    opened_ = true;
    return true;
}

bool FFmpegVideoDecoder::decodeFrame(const std::vector<std::uint8_t>& frame,
                                     std::vector<std::uint8_t>& rgbaOut, int& outW, int& outH) {
    rgbaOut.clear(); outW = outH = 0;
    if (!opened_ || !ctx_) { err_ = "解码器未打开"; return false; }
    if (frame.empty()) return false;

    auto pkt = raiiPacket(av_packet_alloc());
    auto f = raiiFrame(av_frame_alloc());
    bool ok = true;

    pkt->data = const_cast<std::uint8_t*>(frame.data());
    pkt->size = static_cast<int>(frame.size());

    if (bsf_) {
        if (av_bsf_send_packet(bsf_, pkt.get()) < 0) ok = false;
        while (ok) {
            if (av_bsf_receive_packet(bsf_, pkt.get()) < 0) break;
            if (avcodec_send_packet(ctx_, pkt.get()) < 0) { ok = false; break; }
        }
    } else {
        if (avcodec_send_packet(ctx_, pkt.get()) < 0) ok = false;
    }
    if (ok) {
        if (avcodec_receive_frame(ctx_, f.get()) < 0) ok = false;
    }
    if (ok) {
        auto sws = raiiSws(sws_getContext(
            f->width, f->height, (AVPixelFormat)f->format,
            f->width, f->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) { ok = false; err_ = "FFmpeg: swscale 初始化失败"; }
        else {
            rgbaOut.resize(static_cast<std::size_t>(f->width) * f->height * 4);
            std::uint8_t* dst[4] = { rgbaOut.data(), nullptr, nullptr, nullptr };
            int dstStride[4] = { f->width * 4, 0, 0, 0 };
            sws_scale(sws.get(), f->data, f->linesize, 0, f->height, dst, dstStride);
            outW = f->width; outH = f->height;
        }
    }
    if (!ok && err_.empty()) err_ = "FFmpeg: 视频帧解码失败";
    return ok && !rgbaOut.empty();
}

#else  // !SWF_HAVE_FFMPEG —— 桩实现

bool ffmpegDecodeAudio(std::uint8_t /*format*/, std::uint8_t /*rate*/, std::uint8_t /*bits*/,
                       std::uint8_t /*channels*/, const std::vector<std::uint8_t>& /*data*/,
                       PcmData& /*out*/, std::string& err) {
    err = "FFmpeg 未编译进本构建（请用 -DFFMPEG_ROOT 指定预编译 ffmpeg 后重新构建）";
    return false;
}

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;
FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;
bool FFmpegVideoDecoder::open(std::uint8_t /*codecId*/, std::uint16_t /*width*/, std::uint16_t /*height*/,
                              const std::vector<std::uint8_t>& /*extradata*/, std::string& err) {
    err_ = "FFmpeg 未编译进本构建（请用 -DFFMPEG_ROOT 指定预编译 ffmpeg 后重新构建）";
    return false;
}
bool FFmpegVideoDecoder::decodeFrame(const std::vector<std::uint8_t>& /*frame*/,
                                     std::vector<std::uint8_t>& /*rgbaOut*/, int& /*outW*/, int& /*outH*/) {
    err_ = "FFmpeg 未编译进本构建（请用 -DFFMPEG_ROOT 指定预编译 ffmpeg 后重新构建）";
    return false;
}

#endif

}  // namespace swf
