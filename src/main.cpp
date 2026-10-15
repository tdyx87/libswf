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
#include <vector>

#include "swf/Avm1Opcodes.hpp"
#include "swf/Parser.hpp"
#include "swf/TagCode.hpp"
#include "swf/Decoder.hpp"
#include "swf/Render.hpp"

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
    std::string outPath = "out.png";
    std::string outDir = "frames";
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
        else if (a == "--frame" && i + 1 < argc) { frame = std::atoi(argv[++i]); }
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else path = a;
    }

    if (opcodes && path.empty()) { printOpcodes(); return 0; }
    if (path.empty()) { printUsage(); return 1; }

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
