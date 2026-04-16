#pragma once
#include <string>
#include <condition_variable>
#include <memory>
#include <mutex>
#include "cachestream.h"
#include <atomic>
/**
 * swf解析器
 */
class SWF {
public:
    explicit SWF();
    void run(const std::string& file);
    ~SWF();
    void wait_for_head();
    void wait_for_background();
    void GetWindow(int& width, int& height);
    void GetFps(int& fps);
    void GetbackGround(uint8_t& r, uint8_t& g, uint8_t& b)
    {
        r = r_;
        g = g_;
        b = b_;
    }
    void GetNextFrame();
    void ReadTag(const std::vector<uint8_t>& buffer);

private:
    std::atomic<bool> flag{false};
    swf::CacheStream cs;  // 缓存
    uint8_t r_;
    uint8_t g_;
    uint8_t b_;
    uint32_t frame_{0};
};
