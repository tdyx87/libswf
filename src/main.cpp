// swfparser —— 依据逆向还原的 Flash Player SWF 解析链重建的工具
//   头解析  sub_490330 | 解压 sub_4797C0 -> zlib inflate
//   标签头  sub_47A550 | 标签分派 sub_48BA50 | 位流 sub_4791E0/479240 | RECT sub_47A8A0
//
// 用法：
//   swfparser <file.swf>                  dump SWF 结构
//   swfparser <file.swf> --render o.png   解码并用软件光栅器渲染首帧到 PNG
//   swfparser <file.swf> --render o.png --frame N
//   swfparser <file.swf> --render-all outdir  导出所有帧为 outdir/frame_000.png ...
//   swfparser <file.swf> --opcodes        列出 AVM1 操作码表
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include "swf/Avm1Opcodes.hpp"
#include "swf/Parser.hpp"
#include "swf/TagCode.hpp"
#include "swf/Decoder.hpp"
#include "swf/Render.hpp"
#include "swf/Avm1.hpp"
#include "swf/Audio.hpp"
#include "swf/Player.hpp"

// stb_image_write 的实现已在 Render.cpp 中编译（STB_IMAGE_WRITE_IMPLEMENTATION），此处仅用声明。
#include "stb/stb_image_write.h"

#include <cstdint>

static const char* compressionName(swf::Compression c) {
    switch (c) {
        case swf::Compression::Uncompressed: return "Uncompressed (FWS)";
        case swf::Compression::Zlib:         return "zlib (CWS)";
        case swf::Compression::Lzma:         return "LZMA (ZWS)";
    }
    return "?";
}

static void printUsage() {
    std::printf("usage:\n");
    std::printf("  swfparser <file.swf>                       dump SWF structure\n");
    std::printf("  swfparser <file.swf> --render out.png [--frame N]   render a frame to PNG\n");
    std::printf("  swfparser <file.swf> --render-all outdir   export every frame as outdir/frame_NNN.png\n");
    std::printf("  swfparser <file.swf> --opcodes            list AVM1 opcode table\n");
    std::printf("  swfparser <file.swf> --list-avm1          disassemble & print AVM1 scripts + audio/video\n");
    std::printf("  swfparser <file.swf> --extract-audio <id> <out>   export a DefineSound to WAV/raw\n");
    std::printf("  swfparser <file.swf> --wav <id> <out.wav>      decode a DefineSound (MP3/Speex/Nellymoser) to WAV\n");
    std::printf("  swfparser <file.swf> --render-video <id> <outdir>  export decoded video frames as PNGs\n");
    std::printf("  swfparser <file.swf> --play-avm1 <count> <outdir>  AVM1-driven playback -> PNG frames\n");
    std::printf("  swfparser <file.swf> --play-game <count> <outdir> [--click F:X:Y]...  headless full playback via swf::Player\n");
    std::printf("  swfparser <file.swf> --play-game <count> <outdir> [--hover F:X:Y]...  move mouse (no press) at step F\n");
}

static void printOpcodes() {
    std::printf("AVM1 opcode table (sub_4998E0, 85 unique cases, 0x00-0x69)\n");
    const swf::avm1::OpImpl* impl = swf::avm1::opImplTable();
    for (int i = 0; i <= 0x69; ++i) {
        const char* name = swf::avm1::opName(static_cast<std::uint8_t>(i));
        if (std::string(name) == "Unknown") continue;
        const char* handler = "";
        for (int k = 0; k < swf::avm1::opImplCount(); ++k) {
            if (impl[k].code == i) { handler = impl[k].handler; break; }
        }
        std::printf("  0x%02X  %-22s %s\n", i, name, handler);
    }
}

// 主时间轴播放控制器：把 AVM1 的 gotoAndPlay/stop 等映射到逐帧渲染。
struct TimelinePlayer : swf::avm1::ITimeline {
    const swf::Movie& movie;
    int frame = 1;        // 1-based 当前帧
    bool playing = true;
    explicit TimelinePlayer(const swf::Movie& m) : movie(m) { frame = 1; }
    int clamp(int f) const {
        int n = static_cast<int>(movie.frames.size());
        if (f < 1) f = 1;
        if (f > n) f = n;
        return f;
    }
    void gotoAndPlay(int f) override { frame = clamp(f); playing = true; }
    void gotoAndStop(int f) override { frame = clamp(f); playing = false; }
    void play() override { playing = true; }
    void stop() override { playing = false; }
    void nextFrame() override { if (frame < (int)movie.frames.size()) ++frame; }
    void prevFrame() override { if (frame > 1) --frame; }
    void stopAllSounds() override {}
    int currentFrame() const override { return frame; }
    int totalFrames() const override { return static_cast<int>(movie.frames.size()); }
    int framesLoaded() const override { return static_cast<int>(movie.frames.size()); }
};

// 打印解码出的 AVM1 脚本（帧脚本 / 初始化脚本）与音视频统计，便于核对。
static void dumpAvm1(const swf::Movie& movie) {
    std::printf("frameLabels: %zu\n", movie.frameLabels.size());
    for (const auto& kv : movie.frameLabels)
        std::printf("  '%s' -> frame %d\n", kv.first.c_str(), kv.second);
    int n = static_cast<int>(movie.frames.size());
    for (int f = 0; f < n; ++f) {
        for (const auto& sc : movie.frames[f].doActions) {
            std::printf("frame %d DoAction (%zu actions):\n", f, sc.actions.size());
            for (const auto& a : sc.actions)
                std::printf("   0x%02X %s\n", a.code, swf::avm1::opName(a.code));
        }
    }
    for (const auto& kv : movie.initActions) {
        std::printf("DoInitAction spriteId=%u (%zu actions):\n", kv.first, kv.second.actions.size());
        for (const auto& a : kv.second.actions)
            std::printf("   0x%02X %s\n", a.code, swf::avm1::opName(a.code));
    }
    std::printf("sounds: %zu, videos: %zu\n", movie.sounds.size(), movie.videos.size());
    for (const auto& kv : movie.sounds) {
        const auto& s = kv.second;
        std::printf("  Sound id=%u format=%u rate=%u bits=%u ch=%u samples=%u datasize=%zu\n",
                    s.id, s.format, s.rate, s.bits, s.channels, s.sampleCount, s.data.size());
    }
    for (const auto& kv : movie.videos) {
        const auto& v = kv.second;
        std::printf("  VideoStream id=%u frames=%u %ux%u codec=%u frames-data=%zu decoded=%zu\n",
                    v->id, v->numFrames, v->width, v->height, v->codecId, v->frames.size(), v->decoded.size());
    }
}

static const int kSoundRateTable[4] = {5512, 11025, 22050, 44100};

// 抽取一个 DefineSound 为 WAV（未压缩 PCM）/ 裸数据（MP3/压缩）。
static bool doExtractAudio(const swf::Movie& movie, std::uint16_t id, const std::string& out) {
    auto it = movie.sounds.find(id);
    if (it == movie.sounds.end()) { std::fprintf(stderr, "sound id %u not found\n", id); return false; }
    const swf::Sound& s = it->second;
    if (s.format == 2 || s.format == 3) {  // MP3 / 压缩：直接写出原始字节流
        std::FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) return false;
        std::fwrite(s.data.data(), 1, s.data.size(), f);
        std::fclose(f);
        std::printf("wrote raw sound data (%zu bytes) for id %u (format %d)\n", s.data.size(), id, s.format);
        return true;
    }
    int rate = kSoundRateTable[s.rate & 3];
    int bits = s.bits ? 16 : 8;
    int ch = s.channels ? 2 : 1;
    std::uint32_t dataSize = static_cast<std::uint32_t>(s.data.size());
    std::uint16_t blockAlign = static_cast<std::uint16_t>(ch * bits / 8);
    std::uint32_t byteRate = static_cast<std::uint32_t>(rate * blockAlign);
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return false;
    auto w16 = [&](std::uint16_t v) { std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); };
    auto w32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xFF, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(static_cast<std::uint16_t>(ch));
    w32(static_cast<std::uint32_t>(rate)); w32(byteRate); w16(blockAlign); w16(static_cast<std::uint16_t>(bits));
    std::fwrite("data", 1, 4, f); w32(dataSize);
    std::fwrite(s.data.data(), 1, s.data.size(), f);
    std::fclose(f);
    std::printf("wrote WAV %s: rate=%d bits=%d ch=%d (%zu bytes) for sound id %u\n",
                out.c_str(), rate, bits, ch, s.data.size(), id);
    return true;
}

static bool readFile(const std::string& path, std::vector<std::uint8_t>& buf) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    buf.resize(static_cast<std::size_t>(n));
    std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return got == buf.size();
}

int main(int argc, char** argv) {
    std::string path;
    bool opcodes = false, render = false, renderAll = false, inspect = false, runAs = false, play = false;
    bool listAvm1 = false, extractAudio = false, playAvm1 = false;
    bool wav = false, playSound = false, wavStream = false, renderVideo = false;
    bool playGame = false;
    struct ClickSpec { int frame; double x, y; };
    std::vector<ClickSpec> clicks;   // --click <frame>:<x>:<y>（可重复，模拟按下并在 +2 帧释放）
    std::vector<ClickSpec> hovers;   // --hover <frame>:<x>:<y>（可重复，把鼠标移到该位置但不按下）
    std::vector<std::pair<int, int>> keys;   // --key <frame>:<vk>（可重复，在第 frame 步发送一次 keyDown）
    std::string outPath = "out.png";
    std::string outDir = "frames";
    std::string audioOut = "out.wav";
    std::string videoDir = "video";
    std::uint16_t audioId = 0;
    int frame = 0, playCount = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--opcodes" || a == "-o") opcodes = true;
        else if (a == "--render" && i + 1 < argc) { render = true; outPath = argv[++i]; }
        else if (a == "--render-all" && i + 1 < argc) { renderAll = true; outDir = argv[++i]; }
        else if (a == "--inspect") inspect = true;
        else if (a == "--run-as") runAs = true;   // 执行 DoABC(AVM2) 构建运行时显示列表
        else if (a == "--play" && i + 2 < argc) {  // --play <count> <outdir>：逐帧推进并导出动画
            play = true; runAs = true;
            playCount = std::atoi(argv[++i]);
            outDir = argv[++i];
        }
        else if (a == "--list-avm1") listAvm1 = true;   // 反汇编并打印 AVM1 脚本
        else if (a == "--extract-audio" && i + 2 < argc) {  // --extract-audio <id> <out>
            extractAudio = true; audioId = static_cast<std::uint16_t>(std::atoi(argv[++i])); audioOut = argv[++i];
        }
        else if (a == "--wav" && i + 2 < argc) {  // --wav <id> <out.wav>：解码为 WAV
            wav = true; audioId = static_cast<std::uint16_t>(std::atoi(argv[++i])); audioOut = argv[++i];
        }
        else if (a == "--play-sound" && i + 1 < argc) {  // --play-sound <id>：实时播放
            playSound = true; audioId = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
        else if (a == "--wav-stream" && i + 1 < argc) {  // --wav-stream <out.wav>：导出流式背景音
            wavStream = true; audioOut = argv[++i];
        }
        else if (a == "--render-video" && i + 2 < argc) {  // --render-video <id> <outdir>：导出视频帧 PNG
            renderVideo = true; audioId = static_cast<std::uint16_t>(std::atoi(argv[++i])); videoDir = argv[++i];
        }
        else if (a == "--play-avm1" && i + 2 < argc) {  // --play-avm1 <count> <outdir>：AVM1 驱动播放
            playAvm1 = true; playCount = std::atoi(argv[++i]); outDir = argv[++i];
        }
        else if (a == "--play-game" && i + 2 < argc) {  // --play-game <count> <outdir>：真实 swf::Player 全流程播放
            playGame = true; playCount = std::atoi(argv[++i]); outDir = argv[++i];
        }
        else if (a == "--click" && i + 1 < argc) {  // --click <frame>:<x>:<y>
            std::string spec = argv[++i];
            double x = 0, y = 0; int fr = 0;
            if (std::sscanf(spec.c_str(), "%d:%lf:%lf", &fr, &x, &y) == 3) clicks.push_back({fr, x, y});
        }
        else if (a == "--key" && i + 1 < argc) {  // --key <frame>:<vk>：在第 frame 步发送一次 keyDown（vk 为 Windows 虚拟键码）
            std::string spec = argv[++i];
            int fr = 0, vk = 0;
            if (std::sscanf(spec.c_str(), "%d:%d", &fr, &vk) == 2) keys.push_back({fr, vk});
        }
        else if (a == "--hover" && i + 1 < argc) {  // --hover <frame>:<x>:<y>（只移动鼠标，不按下）
            std::string spec = argv[++i];
            double x = 0, y = 0; int fr = 0;
            if (std::sscanf(spec.c_str(), "%d:%lf:%lf", &fr, &x, &y) == 3) hovers.push_back({fr, x, y});
        }
        else if (a == "--frame" && i + 1 < argc) { frame = std::atoi(argv[++i]); }
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else path = a;
    }

    if (opcodes && path.empty()) { printOpcodes(); return 0; }
    if (path.empty()) { printUsage(); return 1; }

    // 全流程无头播放：驱动真实 swf::Player（时间轴 + AVM1 帧脚本 + 精灵首帧脚本 + 鼠标交互）。
    if (playGame) {
        swf::Player player;
        std::string err;
        if (!player.loadFile(path.c_str(), err)) {
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("loaded: %dx%d, %d frames, fps=%.1f\n",
                    player.stageW(), player.stageH(), player.totalFrames(), player.frameRate());
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        // 未显式给 --click 时，默认在第 12 帧点击菜单"开始"按钮（约 110,110）。
        if (clicks.empty()) clicks.push_back({12, 110, 110});
        std::map<int, std::pair<double, double>> down, up;
        for (const auto& c : clicks) {
            down[c.frame] = {c.x, c.y};
            up[c.frame + 2] = {c.x, c.y};   // 按住 2 帧后释放
        }
        char fnbuf[512];
        for (int step = 0; step < playCount; ++step) {
            // 悬停：从指定步开始每帧都重设鼠标位置（显示列表每帧重建，按钮态需持续刷新）。
            for (const auto& hv : hovers)
                if (step >= hv.frame) player.setMouse(hv.x, hv.y);
            auto d = down.find(step);
            if (d != down.end()) { player.setMouse(d->second.first, d->second.second); player.mouseDown(); }
            auto u = up.find(step);
            if (u != up.end()) { player.setMouse(u->second.first, u->second.second); player.mouseUp(); }
            for (const auto& kv : keys)
                if (kv.first == step) player.keyDown(kv.second);
            player.tick();
            std::vector<std::uint8_t> rgba; int W = 0, H = 0;
            if (player.render(rgba, W, H)) {
                std::snprintf(fnbuf, sizeof(fnbuf), "%s/p_%04d.png", outDir.c_str(), step);
                stbi_write_png(fnbuf, W, H, 4, rgba.data(), W * 4);
            }
            std::printf("step=%d frame=%d\n", step, player.currentFrame());
        }
        std::printf("played %d steps -> %s/\n", playCount, outDir.c_str());
        return 0;
    }

    // AVM1 / 音视频 导出模式
    if (listAvm1 || extractAudio || playAvm1 || wav || playSound || wavStream || renderVideo) {
        std::vector<std::uint8_t> buf;
        if (!readFile(path, buf)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
        swf::Movie movie;
        std::string err;
        if (!swf::decodeMovie(buf.data(), buf.size(), movie, err)) {
            std::fprintf(stderr, "decode failed: %s\n", err.c_str());
            return 1;
        }
        if (listAvm1) { dumpAvm1(movie); return 0; }
        if (extractAudio) {
            if (!doExtractAudio(movie, audioId, audioOut)) return 1;
            return 0;
        }
        if (wav || playSound) {
            auto it = movie.sounds.find(audioId);
            if (it == movie.sounds.end()) { std::fprintf(stderr, "sound id %u not found\n", audioId); return 1; }
            swf::PcmData pcm;
            if (!swf::decodeSound(it->second, pcm, err)) {
                std::fprintf(stderr, "decode sound %u failed: %s\n", audioId, err.c_str());
                return 1;
            }
            if (wav) {
                if (!swf::writeWav(audioOut, pcm)) { std::fprintf(stderr, "writeWav failed\n"); return 1; }
                std::printf("decoded sound id %u (%s, %dHz, %dch) -> %s (%zu samples)\n",
                            audioId, swf::soundFormatName(it->second.format), pcm.rate, pcm.channels,
                            audioOut.c_str(), pcm.samples.size());
                return 0;
            }
            // playSound：实时播放
            if (!swf::playPcm(pcm, err)) { std::fprintf(stderr, "play failed: %s\n", err.c_str()); return 1; }
            std::printf("played sound id %u (%s, %dHz, %dch, %zu samples)\n",
                        audioId, swf::soundFormatName(it->second.format), pcm.rate, pcm.channels, pcm.samples.size());
            return 0;
        }
        if (wavStream) {
            swf::PcmData pcm;
            if (!swf::decodeStreamSound(movie, pcm, err)) {
                std::fprintf(stderr, "decode stream sound failed: %s\n", err.c_str());
                return 1;
            }
            if (!swf::writeWav(audioOut, pcm)) { std::fprintf(stderr, "writeWav failed\n"); return 1; }
            std::printf("decoded streaming audio -> %s (%dHz, %dch, %zu samples)\n",
                        audioOut.c_str(), pcm.rate, pcm.channels, pcm.samples.size());
            return 0;
        }
        if (renderVideo) {
            auto vit = movie.videos.find(audioId);
            if (vit == movie.videos.end() || !vit->second) {
                std::fprintf(stderr, "video id %u not found\n", audioId); return 1;
            }
            auto vs = vit->second;
            std::error_code ec; std::filesystem::create_directories(videoDir, ec);
            int n = 0; char fn[256];
            for (auto& kv : vs->decoded) {
                auto& bmp = kv.second; if (!bmp) continue;
                std::snprintf(fn, sizeof fn, "%s/frame_%04d.png", videoDir.c_str(), (int)kv.first);
                stbi_write_png(fn, bmp->width, bmp->height, 4, bmp->rgba.data(), bmp->width * 4);
                ++n;
            }
            if (n == 0) {
                std::printf("video id %u: codecId=%u (%ux%u) 未能解码（%s）—— 需启用 FFmpeg 后端\n",
                            audioId, vs->codecId, vs->width, vs->height,
                            vs->codecId == 3 ? "无已解码帧" : "H.263/VP6/AVC 未配置 FFmpeg 或未包含已解码帧");
            } else {
                std::printf("exported %d video frames (codecId=%u %ux%u) -> %s/\n",
                            n, vs->codecId, vs->width, vs->height, videoDir.c_str());
            }
            return 0;
        }
        if (playAvm1) {
            if (movie.frames.empty()) { std::fprintf(stderr, "no frames to render\n"); return 1; }
            TimelinePlayer player(movie);
            swf::avm1::Machine machine;
            machine.timeline = &player;
            machine.frameLabels = movie.frameLabels;
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            char fnbuf[512];
            for (int step = 0; step < playCount; ++step) {
                int idx = player.frame - 1;
                if (idx >= 0 && idx < (int)movie.frames.size())
                    for (auto& sc : movie.frames[idx].doActions) machine.run(sc);
                std::snprintf(fnbuf, sizeof(fnbuf), "frame_%04d.png", step);
                std::filesystem::path outp = std::filesystem::path(outDir) / fnbuf;
                if (!swf::renderMovieToPNG(movie, idx, outp.string().c_str())) {
                    std::fprintf(stderr, "render failed at step %d\n", step);
                    return 1;
                }
                if (player.playing) {
                    if (player.frame >= (int)movie.frames.size()) player.frame = 1;
                    else ++player.frame;
                }
            }
            std::printf("played %d AVM1-driven frames -> %s/\n", playCount, outDir.c_str());
            return 0;
        }
    }

    // 渲染模式：解码 + 光栅化（--run-as 也走这条路径以执行 AVM2）
    if (render || renderAll || inspect || runAs) {
        std::vector<std::uint8_t> buf;
        if (!readFile(path, buf)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
        swf::Movie movie;
        std::string err;
        if (!swf::decodeMovie(buf.data(), buf.size(), movie, err)) {
            std::fprintf(stderr, "decode failed: %s\n", err.c_str());
            return 1;
        }
        if (movie.frames.empty()) { std::fprintf(stderr, "no frames to render\n"); return 1; }

        if (runAs) {
            if (movie.abcFiles.empty()) {
                std::printf("no DoABC in file; nothing to execute\n");
            } else {
                std::printf("running AVM2 scripts (%zu abc)...\n", movie.abcFiles.size());
                auto vms = swf::fvm::runMovieScripts(movie);
                std::printf("AVM2 done: vms=%zu, runtime stage nodes=%zu, symbolClasses=%zu\n",
                            vms.size(), movie.runtimeStage.size(), movie.symbolClasses.size());

                if (play && !movie.runtimeStage.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(outDir, ec);
                    char fnbuf[512];
                    for (int f = 0; f < playCount; ++f) {
                        for (auto& vm : vms) vm->tick();   // 推进所有时间轴 + 派发 enterFrame 等
                        std::snprintf(fnbuf, sizeof(fnbuf), "frame_%03d.png", f);
                        std::filesystem::path outp = std::filesystem::path(outDir) / fnbuf;
                        if (!swf::renderMovieToPNG(movie, 0, outp.string().c_str())) {
                            std::fprintf(stderr, "render frame %d failed\n", f);
                            return 1;
                        }
                    }
                    std::printf("played %d frames -> %s/\n", playCount, outDir.c_str());
                    return 0;
                }
            }
            if (!(render || renderAll)) return 0;
        }

        if (inspect) {
            std::printf("characters: %zu\n", movie.characters.size());
            for (const auto& kv : movie.characters) {
                const auto& ch = kv.second;
                if (ch->kind == swf::Character::Kind::Shape && ch->shape) {
                    std::printf("  [%5u] Shape  fills=%zu lines=%zu  fillKinds=[",
                                kv.first, ch->shape->fillStyles.size(), ch->shape->lineStyles.size());
                    for (std::size_t k = 0; k < ch->shape->fillStyles.size(); ++k) {
                        int kk = static_cast<int>(ch->shape->fillStyles[k].kind);
                        std::printf("%d%s", kk, (k + 1 < ch->shape->fillStyles.size()) ? "," : "");
                    }
                    std::printf("] bmpIds=[");
                    for (std::size_t k = 0; k < ch->shape->fillStyles.size(); ++k) {
                        if (ch->shape->fillStyles[k].kind == swf::FillKind::Bitmap)
                            std::printf("%u%s", ch->shape->fillStyles[k].bitmapId, (k + 1 < ch->shape->fillStyles.size()) ? "," : "");
                    }
                    std::printf("]\n");
                } else if (ch->kind == swf::Character::Kind::Bitmap && ch->bitmap) {
                    std::printf("  [%5u] Bitmap %dx%d (rgba %zu bytes)\n", kv.first,
                                ch->bitmap->width, ch->bitmap->height, ch->bitmap->rgba.size());
                } else if (ch->kind == swf::Character::Kind::Sprite) {
                    std::printf("  [%5u] Sprite frames=%zu", kv.first, ch->spriteFrames.size());
                    for (std::size_t k = 0; k < ch->spriteFrames.size() && k < 4; ++k) {
                        std::printf(" [f%zu items=%zu]", k, ch->spriteFrames[k].items.size());
                    }
                    std::printf("\n");
                    if (kv.first <= 2) {  // 详细打印前几个精灵的帧内容
                        for (std::size_t k = 0; k < ch->spriteFrames.size() && k < 3; ++k) {
                            std::printf("        sprite f%zu:", k);
                            for (const auto& it : ch->spriteFrames[k].items)
                                std::printf(" d%u:k%d", it.depth,
                                            it.character ? static_cast<int>(it.character->kind) : -1);
                            std::printf("\n");
                        }
                    }
                } else if (ch->kind == swf::Character::Kind::Font && ch->font) {
                    int nonEmpty = 0;
                    for (const auto& gs : ch->font->shapes) if (gs && !gs->fillPaths.empty() && !gs->fillPaths[0].empty()) ++nonEmpty;
                    std::printf("  [%5u] Font glyphs=%zu nonEmpty=%d codes=%zu\n",
                                kv.first, ch->font->shapes.size(), nonEmpty, ch->font->codeToGlyph.size());
                } else if (ch->kind == swf::Character::Kind::Text && ch->text) {
                    std::printf("  [%5u] Text glyphs=%zu mat(sx=%.3f sy=%.3f)\n",
                                kv.first, ch->text->glyphs.size(), ch->text->textMatrix.sx, ch->text->textMatrix.sy);
                    for (std::size_t gi = 0; gi < ch->text->glyphs.size() && gi < 12; ++gi) {
                        const auto& tg = ch->text->glyphs[gi];
                        std::printf("        g%zu font=%u idx=%u adv=%d pos=(%d,%d) color=#%02X%02X%02X\n",
                                    gi, tg.fontId, tg.glyphIndex, tg.advance, tg.x, tg.y,
                                    tg.color.r, tg.color.g, tg.color.b);
                    }
                } else if (ch->kind == swf::Character::Kind::EditText && ch->editText) {
                    std::printf("  [%5u] EditText font=%u h=%u color=#%02X%02X%02X wrap=%d multi=%d text=\"%s\"\n",
                                kv.first, ch->editText->fontId, ch->editText->fontHeight,
                                ch->editText->color.r, ch->editText->color.g, ch->editText->color.b,
                                ch->editText->wordWrap ? 1 : 0, ch->editText->multiline ? 1 : 0,
                                ch->editText->text.c_str());
                } else if (ch->kind == swf::Character::Kind::Button && ch->button) {
                    auto cids = [](const std::vector<swf::DisplayItem>& v) {
                        std::string s;
                        for (const auto& it : v) { if (!s.empty()) s += ","; s += std::to_string(it.characterId); }
                        return s;
                    };
                    std::printf("  [%5u] Button up=[%s] over=[%s] down=[%s] hit=[%s]\n",
                                kv.first, cids(ch->button->up).c_str(), cids(ch->button->over).c_str(),
                                cids(ch->button->down).c_str(), cids(ch->button->hit).c_str());
                } else {
                    std::printf("  [%5u] ?\n", kv.first);
                }
            }
            std::printf("frames: %zu\n", movie.frames.size());
            if (!movie.abcFiles.empty()) {
                for (std::size_t fi = 0; fi < movie.abcFiles.size(); ++fi) {
                    const auto& a = movie.abcFiles[fi];
                    std::printf("abc[%zu]: v%d.%d methods=%zu bodies=%zu instances=%zu classes=%zu scripts=%zu strings=%zu multinames=%zu\n",
                                fi, a.major, a.minor, a.methods.size(), a.methodBodies.size(),
                                a.instances.size(), a.classes.size(), a.scripts.size(),
                                a.strings.size(), a.multinames.size());
                }
            }
            for (std::size_t f = 0; f < movie.frames.size(); ++f) {
                std::printf("  frame %zu items=%zu:", f, movie.frames[f].items.size());
                for (const auto& it : movie.frames[f].items) {
                    int kd = it.character ? static_cast<int>(it.character->kind) : -1;
                    // 找角色 id（用于关联精灵）
                    std::uint16_t cid = 0;
                    for (const auto& kv : movie.characters)
                        if (kv.second == it.character) { cid = kv.first; break; }
                    std::printf(" d%u:k%d#%u", it.depth, kd, cid);
                    if (kd == 2 && it.character) {
                        const auto& sf = it.character->spriteFrames;
                        std::printf("(spriteFrames=%zu)", sf.size());
                    }
                    if (it.clipDepth) std::printf("(clip->%u)", it.clipDepth);
                }
                std::printf("\n");
            }
            return 0;
        }

        if (renderAll) {
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            std::size_t count = 0;
            char fnbuf[512];
            for (std::size_t f = 0; f < movie.frames.size(); ++f) {
                std::snprintf(fnbuf, sizeof(fnbuf), "frame_%03zu.png", f);
                std::filesystem::path outp = std::filesystem::path(outDir) / fnbuf;
                if (!swf::renderMovieToPNG(movie, static_cast<int>(f), outp.string().c_str())) {
                    std::fprintf(stderr, "render frame %zu failed\n", f);
                    return 1;
                }
                ++count;
            }
            std::printf("rendered %zu frames -> %s/\n", count, outDir.c_str());
            return 0;
        }

        if (frame >= static_cast<int>(movie.frames.size())) frame = 0;
        if (!swf::renderMovieToPNG(movie, frame, outPath.c_str())) {
            std::fprintf(stderr, "render failed\n");
            return 1;
        }
        std::printf("rendered frame %d -> %s  (%d frames total)\n",
                    frame, outPath.c_str(), static_cast<int>(movie.frames.size()));
        return 0;
    }

    // dump 模式
    swf::Parser parser;
    swf::ParseResult r;
    int rc = parser.parseFile(path, r);

    std::printf("file        : %s\n", path.c_str());
    if (!r.error.empty() && !r.ok) {
        std::printf("error       : %s (rc=%d)\n", r.error.c_str(), rc);
        return 1;
    }

    std::printf("signature   : %s\n", r.header.signature);
    std::printf("compression : %s\n", compressionName(r.header.compression));
    std::printf("version     : %u\n", r.header.version);
    std::printf("fileLength  : %u (actual body %zu)\n", r.header.fileLength, r.bodySize);
    std::printf("frameSize   : (%d,%d)-(%d,%d)  %dx%d\n",
                r.body.frameSize.xMin, r.body.frameSize.yMin,
                r.body.frameSize.xMax, r.body.frameSize.yMax,
                r.body.frameSize.xMax - r.body.frameSize.xMin,
                r.body.frameSize.yMax - r.body.frameSize.yMin);
    std::printf("frameRate   : %.2f fps\n", r.body.frameRate());
    std::printf("frameCount  : %u\n", r.body.frameCount);
    std::printf("tags        : %zu (definition %zu / execution %zu)\n",
                r.tags.size(), r.definitionTagCount(), r.frameTagCount());
    std::printf("\n-- tag list --\n");
    for (const auto& t : r.tags) {
        std::printf("  [%5u] %-28s len=%-8u %s\n",
                    t.code, swf::tagName(t.code), t.length, t.note.c_str());
    }
    if (opcodes) { std::printf("\n"); printOpcodes(); }
    return 0;
}
