#include "swf.h"
#include "filestream.h"
#include "decompress.h"
#include "cachestream.h"
SWF::SWF() {}
SWF::~SWF() {}
void SWF::wait_for_head()
{
    while (!flag.load(std::memory_order_acquire)) std::this_thread::yield();
}

void SWF::wait_for_background()
{
    while (!flag.load(std::memory_order_acquire)) std::this_thread::yield();
}

void SWF::GetWindow(int& width, int& height)
{
    width = cs.GetWidth();
    height = cs.GetHeight();
}
void SWF::GetFps(int& fps)
{
    fps = cs.GetFps();
}
void SWF::GetNextFrame()
{
    // 显示当前帧的内容
}
void SWF::ReadTag(TagType tag, const std::vector<uint8_t>& buffer)
{
    swf::CacheStream cs(buffer);
    switch (tag) {
        case TagType::TAG_SETBACKGROUNDCOLOR: {
            cs.read_uint8(r_);
            cs.read_uint8(g_);
            cs.read_uint8(b_);
        } break;
    }
}
void SWF::run(const std::string& file)
{
    FileStream stream(file);
    int ret = -1;
    int version = 0;
    uint32_t datasize = 0;
    swf::ErrorCode cs_ret = swf::ErrorCode::SUCCESS;
    ret = stream.read(8, [&](std::vector<uint8_t> buffer, uint32_t size) {
        cs_ret = cs.parse(buffer);
    });
    if (cs_ret == swf::ErrorCode::INVALID_SWF) {
        return;
    }
    cs.set_head_complete(
        [&]() { flag.store(true, std::memory_order_release); });
    auto ret2 = [&](const std::vector<uint8_t>& buffer, uint32_t length,
                    TagType& tag) {
        std::vector<uint8_t> b(buffer.begin(), buffer.begin() + length);
        ReadTag(tag, b);
        return 0;
    };
    cs.set_tag_parser(ret2);
    if (cs.mode() == swf::CompressMode::ZLIB) {
        ZlibDecompress zlib;
        int compressret = zlib.Init();
        if (compressret != Z_OK) return;
        do {
            ret = stream.read(
                1024, [&](std::vector<uint8_t> buffer, uint32_t size) {
                    std::vector<uint8_t> realbuffer;
                    compressret = zlib.Decompress(buffer, realbuffer);
                    cs.parse(realbuffer);
                });
            if (compressret == -1) {
                // 解压失败
                break;
            }

        } while (ret != 0 && compressret != Z_STREAM_END);

        ret = zlib.End();
    }
}
