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
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "swf/Avm1Opcodes.hpp"
#include "swf/Avm2Opcodes.hpp"
#include "swf/Fvm.hpp"
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
    std::printf("  swfparser --avm2-opcodes                  list AVM2 opcode table + interpreter coverage\n");
    std::printf("  swfparser <file.swf> --list-avm1          disassemble & print AVM1 scripts + audio/video\n");
    std::printf("  swfparser <file.swf> --extract-audio <id> <out>   export a DefineSound to WAV/raw\n");
    std::printf("  swfparser <file.swf> --wav <id> <out.wav>      decode a DefineSound (MP3/Speex/Nellymoser) to WAV\n");
    std::printf("  swfparser <file.swf> --render-video <id> <outdir>  export decoded video frames as PNGs\n");
    std::printf("  swfparser <file.swf> --play-avm1 <count> <outdir>  AVM1-driven playback -> PNG frames\n");
    std::printf("  swfparser <file.swf> --play <count> <outdir> [--click F:X:Y] [--hover F:X:Y]  AVM2 playback -> PNG frames\n");
    std::printf("  swfparser <file.swf> --play-game <count> <outdir> [--click F:X:Y]...  headless full playback via swf::Player\n");
    std::printf("  swfparser <file.swf> --play-game <count> <outdir> [--hover F:X:Y]...  move mouse (no press) at step F\n");
    std::printf("  [--size WxH]  re-rasterize each frame at WxH (vector/text stay sharp when upscaled)\n");
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

// AVM2 规范 opcode 表 + 解释器覆盖度（S=标准 E=扩展 B=分支 R=返回 V=运行时栈）。
static void printAvm2Opcodes() {
    namespace a = swf::avm2;
    bool impl[256] = {};
    for (std::uint8_t op : swf::fvm::implementedOpcodes()) impl[op] = true;
    const auto standard = a::standardOpcodes();
    std::printf("AVM2 opcode table (avmplus 256-slot numbering; %zu standard opcodes)\n",
                standard.size());
    std::printf("  0xNN  mnemonic              operands          stack       flags  impl\n");
    for (int i = 0; i < 256; ++i) {
        const a::OpInfo& info = a::opInfo(static_cast<std::uint8_t>(i));
        if (!info.valid()) continue;
        std::string flags;
        flags += (info.flags & a::FlagStandard) ? 'S' : 'E';
        if (info.flags & a::FlagBranch) flags += 'B';
        if (info.flags & a::FlagReturn) flags += 'R';
        if (info.flags & a::FlagThrow) flags += 'T';
        if (info.flags & a::FlagDebug) flags += 'D';
        if (info.flags & a::FlagVariableStack) flags += 'V';
        char stack[48];
        if (info.variableStack())
            std::snprintf(stack, sizeof(stack), "%d+%d*n->%d",
                          info.popBase, info.popPerArg, info.push);
        else
            std::snprintf(stack, sizeof(stack), "%d->%d", info.popBase, info.push);
        std::printf("  0x%02X  %-20s %-16s %-11s %-6s %s\n",
                    i, info.name, a::operandName(info.operand), stack,
                    flags.c_str(), impl[i] ? "yes" : "-");
    }
    std::size_t missing = 0;
    std::string missingList;
    for (std::uint8_t op : standard) {
        if (impl[op]) continue;
        char b[16];
        std::snprintf(b, sizeof(b), " 0x%02X", op);
        missingList += b;
        ++missing;
    }
    std::printf("\nstandard opcode coverage: %zu/%zu implemented\n",
                standard.size() - missing, standard.size());
    if (missing) std::printf("missing:%s\n", missingList.c_str());
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

// PNG 编码（zlib 压缩）耗时通常是软件光栅的数倍。导出逐帧序列时把编码交给后台
// 线程，与下一帧的渲染重叠执行；编码只读传入的 RGBA 缓冲，与渲染无数据竞争。
static std::future<bool> writePngAsync(std::string path, std::vector<std::uint8_t> rgba,
                                       int W, int H) {
    return std::async(std::launch::async,
                      [path = std::move(path), rgba = std::move(rgba), W, H] {
                          return stbi_write_png(path.c_str(), W, H, 4, rgba.data(), W * 4) != 0;
                      });
}

// 双缓冲版：缓冲由播放循环持有并复用，避免每帧重新分配整幅 RGBA。
static std::future<bool> writePngAsync(std::string path,
                                       std::shared_ptr<std::vector<std::uint8_t>> rgba,
                                       int W, int H) {
    return std::async(std::launch::async,
                      [path = std::move(path), rgba = std::move(rgba), W, H] {
                          return stbi_write_png(path.c_str(), W, H, 4, rgba->data(), W * 4) != 0;
                      });
}

int main(int argc, char** argv) {
    std::string path;
    bool opcodes = false, avm2Opcodes = false;
    bool render = false, renderAll = false, inspect = false, runAs = false, play = false;
    bool listAvm1 = false, extractAudio = false, playAvm1 = false;
    bool wav = false, playSound = false, wavStream = false, renderVideo = false;
    bool playGame = false, noRender = false;
    struct ClickSpec { int frame; double x, y; };
    std::vector<ClickSpec> clicks;   // --click <frame>:<x>:<y>（可重复，模拟按下并在 +2 帧释放）
    std::vector<ClickSpec> hovers;   // --hover <frame>:<x>:<y>（可重复，把鼠标移到该位置但不按下）
    // --drag <down>:<up>:<x1>:<y1>:<x2>:<y2>：按下后逐帧从 (x1,y1) 移到 (x2,y2)，第 up 帧释放
    struct DragSpec { int down, up; double x1, y1, x2, y2; };
    std::vector<DragSpec> drags;
    std::vector<std::pair<int, int>> keys;   // --key <frame>:<vk>（可重复，在第 frame 步发送一次 keyDown）
    std::string outPath = "out.png";
    std::string outDir = "frames";
    std::string audioOut = "out.wav";
    std::string videoDir = "video";
    std::uint16_t audioId = 0;
    int frame = 0, playCount = 0;
    int targetW = 0, targetH = 0;   // --size WxH：按设备分辨率重新光栅化
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--opcodes" || a == "-o") opcodes = true;
        else if (a == "--avm2-opcodes") avm2Opcodes = true;
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
        else if (a == "--no-render") noRender = true;   // 只推进时间轴与交互，不导出 PNG
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
        else if (a == "--drag" && i + 1 < argc) {  // --drag <down>:<up>:<x1>:<y1>:<x2>:<y2>
            std::string spec = argv[++i];
            DragSpec ds{};
            if (std::sscanf(spec.c_str(), "%d:%d:%lf:%lf:%lf:%lf",
                            &ds.down, &ds.up, &ds.x1, &ds.y1, &ds.x2, &ds.y2) == 6)
                drags.push_back(ds);
        }
        else if (a == "--frame" && i + 1 < argc) { frame = std::atoi(argv[++i]); }
        else if (a == "--size" && i + 1 < argc) {  // --size <W>x<H>：高分辨率重新光栅化
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                targetW = w; targetH = h;
            }
        }
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else path = a;
    }

    if (avm2Opcodes && path.empty()) { printAvm2Opcodes(); return 0; }
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
        // 帧序列导出以吞吐优先；zlib 级别 8 对每帧 4MB RGBA 是主要 CPU 开销。
        // 可用 SWF_PNG_LEVEL 覆盖（例如归档时设 8）。
        stbi_write_png_compression_level = std::getenv("SWF_PNG_LEVEL")
            ? std::atoi(std::getenv("SWF_PNG_LEVEL")) : 1;
        // stb 默认对每行试 5 种 PNG filter 并计算熵择优；帧序列用 None filter 足够，
        // 省掉约 5 倍的逐行扫描。需要更小体积时设 SWF_PNG_FILTER=-1 恢复自适应。
        stbi_write_force_png_filter = std::getenv("SWF_PNG_FILTER")
            ? std::atoi(std::getenv("SWF_PNG_FILTER")) : 0;
        const bool perf = std::getenv("SWF_PERF") != nullptr;
        using Clock = std::chrono::steady_clock;
        double tTick = 0.0, tRender = 0.0, tPng = 0.0;
        // 多套 RGBA 缓冲轮换：渲染写当前槽时，其它槽可在后台并行编码；
        // 槽数大于「单帧编码时间 / 单帧渲染时间」即可把 PNG 完全隐藏。
        int slots = 4;
        if (const char* se = std::getenv("SWF_PNG_SLOTS")) {
            const int v = std::atoi(se);
            if (v >= 1 && v <= 8) slots = v;
        }
        std::vector<std::shared_ptr<std::vector<std::uint8_t>>> buffers(
            static_cast<std::size_t>(slots));
        for (auto& b : buffers) b = std::make_shared<std::vector<std::uint8_t>>();
        std::vector<std::future<bool>> pendingPng(static_cast<std::size_t>(slots));
        for (int step = 0; step < playCount; ++step) {
            // 悬停：从指定步开始每帧都重设鼠标位置（显示列表每帧重建，按钮态需持续刷新）。
            for (const auto& hv : hovers)
                if (step >= hv.frame) player.setMouse(hv.x, hv.y);
            // --drag：按下后逐帧移动指针（拖动靠移动驱动），到期释放。
            for (const auto& dg : drags) {
                if (step < dg.down || step > dg.up) continue;
                double t = (dg.up > dg.down) ? double(step - dg.down) / double(dg.up - dg.down) : 1.0;
                double dx = dg.x1 + (dg.x2 - dg.x1) * t;
                double dy = dg.y1 + (dg.y2 - dg.y1) * t;
                if (step == dg.down) { player.setMouse(dx, dy); player.mouseDown(); }
                else if (step == dg.up) { player.setMouse(dx, dy); player.mouseUp(); }
                else player.setMouse(dx, dy);
            }
            auto d = down.find(step);
            if (d != down.end()) { player.setMouse(d->second.first, d->second.second); player.mouseDown(); }
            auto u = up.find(step);
            if (u != up.end()) { player.setMouse(u->second.first, u->second.second); player.mouseUp(); }
            for (const auto& kv : keys)
                if (kv.first == step) player.keyDown(kv.second);
            auto t0 = Clock::now();
            player.tick();
            if (perf) tTick += std::chrono::duration<double>(Clock::now() - t0).count();
            std::printf("step=%d frame=%d\n", step, player.currentFrame());
            if (noRender) continue;
            const std::size_t s = static_cast<std::size_t>(step % slots);
            auto& cur = buffers[s];
            // 该槽位上一次（slots 帧前）的编码必须先结束，才能覆盖缓冲。
            if (pendingPng[s].valid()) {
                auto t1 = Clock::now();
                pendingPng[s].get();
                if (perf) tPng += std::chrono::duration<double>(Clock::now() - t1).count();
            }
            int W = 0, H = 0;
            auto t2 = Clock::now();
            const bool ok = player.render(*cur, W, H, targetW, targetH);
            if (perf) tRender += std::chrono::duration<double>(Clock::now() - t2).count();
            if (ok) {
                std::snprintf(fnbuf, sizeof(fnbuf), "%s/p_%04d.png", outDir.c_str(), step);
                pendingPng[s] = writePngAsync(fnbuf, cur, W, H);
            }
        }
        for (auto& f : pendingPng) if (f.valid()) f.get();
        if (perf)
            std::fprintf(stderr, "[perf] steps=%d tick=%.3fs render=%.3fs png_wait=%.3fs\n",
                         playCount, tTick, tRender, tPng);
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
                // 纯 ASCII：此前该行在 MSVC /utf-8 下触发 C4819，
                // 导致格式串解析异常并报 C4474（参数数量不匹配）。
                std::printf("video id %u: codecId=%u (%u x %u) not decoded (%s) - FFmpeg backend required\n",
                            audioId, vs->codecId, vs->width, vs->height,
                            vs->codecId == 3 ? "no decoded frames"
                                : "H.263/VP6/AVC needs FFmpeg or has no decoded frames");
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
                if (std::getenv("SWF_AS_SYMBOLS"))   // 临时诊断：打印 SymbolClass 映射
                    for (const auto& kv : movie.symbolClasses)
                        std::printf("[symbol] %s -> char %u\n", kv.first.c_str(), kv.second);

                if (play && !movie.runtimeStage.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(outDir, ec);
                    // 输入调度：--click F:X:Y 在 F 帧按下、F+2 帧释放；--hover F:X:Y 从 F 帧起保持指针位置
                    std::map<int, std::pair<double, double>> down, up;
                    for (const auto& c : clicks) {
                        down[c.frame] = {c.x, c.y};
                        up[c.frame + 2] = {c.x, c.y};
                    }
                    char fnbuf[512];
                    for (int f = 0; f < playCount; ++f) {
                        for (const auto& hv : hovers)
                            if (f >= hv.frame)
                                for (auto& vm : vms) vm->setMousePosition(hv.x, hv.y);
                        // --drag：按下后逐帧移动指针（拖动靠移动驱动），到期释放
                        for (const auto& dg : drags) {
                            if (f < dg.down || f > dg.up) continue;
                            double t = (dg.up > dg.down)
                                           ? double(f - dg.down) / double(dg.up - dg.down)
                                           : 1.0;
                            double dx = dg.x1 + (dg.x2 - dg.x1) * t;
                            double dy = dg.y1 + (dg.y2 - dg.y1) * t;
                            for (auto& vm : vms) {
                                if (f == dg.down) vm->mouseDown(dx, dy);
                                else if (f == dg.up) vm->mouseUp(dx, dy);
                                else vm->setMousePosition(dx, dy);
                            }
                        }
                        auto d = down.find(f);
                        if (d != down.end())
                            for (auto& vm : vms) vm->mouseDown(d->second.first, d->second.second);
                        auto u = up.find(f);
                        if (u != up.end())
                            for (auto& vm : vms) vm->mouseUp(u->second.first, u->second.second);
                        for (auto& vm : vms) vm->tick();   // 推进所有时间轴 + 派发 enterFrame 等
                        if (noRender) continue;
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

        // 含 DoABC 的影片：静态帧列表只反映"设计期"放置，像 service_error_mc 这样由
        // AS3 首帧脚本控制可见性的对象必须执行脚本，才能与 Flash Player 播放的首帧一致。
        // 仅首帧启用运行时显示树（第 0 帧初始化 + 一次 tick 执行 addFrameScript(0)），
        // 其余帧仍按静态帧渲染，避免改变 --render-all / --frame N 的既有语义。
        bool firstFrameRuntime = false;
        if ((render || renderAll) && !runAs && !movie.abcFiles.empty()) {
            auto vms = swf::fvm::runMovieScripts(movie, 0);
            for (auto& vm : vms) vm->tick();
            firstFrameRuntime = !movie.runtimeStage.empty();
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
            std::future<bool> pendingPng;
            for (std::size_t f = 0; f < movie.frames.size(); ++f) {
                if (f > 0 && firstFrameRuntime) movie.runtimeStage.clear();
                std::snprintf(fnbuf, sizeof(fnbuf), "frame_%03zu.png", f);
                std::filesystem::path outp = std::filesystem::path(outDir) / fnbuf;
                std::vector<std::uint8_t> next;
                int w = 0, h = 0;
                if (!swf::renderMovieToBuffer(movie, static_cast<int>(f), next, w, h)) {
                    std::fprintf(stderr, "render frame %zu failed\n", f);
                    return 1;
                }
                if (pendingPng.valid()) pendingPng.get();   // 上一帧编码完成后再提交
                pendingPng = writePngAsync(outp.string(), std::move(next), w, h);
                ++count;
            }
            if (pendingPng.valid()) pendingPng.get();
            std::printf("rendered %zu frames -> %s/\n", count, outDir.c_str());
            return 0;
        }

        if (frame >= static_cast<int>(movie.frames.size())) frame = 0;
        if (frame > 0 && firstFrameRuntime) movie.runtimeStage.clear();
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
    // rc<0 时 error 可能为空（截断的标签流），只看 error 会漏报失败。
    if (rc < 0 || !r.error.empty()) {
        std::printf("error       : %s (rc=%d)\n",
                    r.error.empty() ? "parse failed" : r.error.c_str(), rc);
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
    if (avm2Opcodes) { std::printf("\n"); printAvm2Opcodes(); }
    if (opcodes) { std::printf("\n"); printOpcodes(); }
    return 0;
}
