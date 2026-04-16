#pragma once
/**
 * 解析缓存系统
 */
#include <stdint.h>
#include <vector>
#include <functional>
#include "tagdefine.h"

struct FIXED8 {
    union {
        float f;
        struct i {
            uint16_t a;
            uint16_t b;
        } i;
    };
};
namespace swf {
struct RECT {
    uint32_t nbits;
    int32_t xmin;
    int32_t xmax;
    int32_t ymin;
    int32_t ymax;
};
enum class ErrorCode { SUCCESS = 0, MOREDATA = 1, INVALID_SWF = -1 };
enum class CompressMode { UNKNOWN = 0, NO = 1, ZLIB = 2, LAMA = 3 };
class CacheStream {
    enum class ParseState {
        SWF_NONE,
        SWF_SIGNATURE,
        SWF_VERSION,
        SWF_FILELENGTH,
        SWF_FRAMESIZE,
        SWF_FRAMERATE,
        SWF_FRAMECOUNT,
        SWF_TAG,
        SWF_TAG_HEADR,
        SWF_TAG_LONG_HEADER,
        SWF_TAG_CONTENT,
    };
    using head_complete_func = std::function<void()>;
    using tag_parser =
        std::function<int(const std::vector<uint8_t>&, uint32_t, TagType&)>;

public:
    CacheStream(const std::vector<uint8_t>& buffer);
    CacheStream();
    ~CacheStream();
    void Init(uint32_t capacity);
    ErrorCode parse(const std::vector<uint8_t>& cache);
    void set_head_complete(head_complete_func func)
    {
        head_complete_ = func;
    }
    void set_tag_parser(tag_parser parser)
    {
        tag_parser_ = parser;
    }
    bool has_parse_head();
    CompressMode mode() const
    {
        return mode_;
    }
    int GetWidth() const
    {
        return (frameSize_.xmax - frameSize_.xmin) / 20;
    }
    int GetHeight() const
    {
        return (frameSize_.ymax - frameSize_.ymin) / 20;
    }
    int GetFps() const
    {
        return static_cast<int>(frameRate_);
    }
    void read_ub(uint32_t& value, uint32_t nbits);
    void read_sb(int32_t& value, uint32_t nbits);
    void read_uint16(uint16_t& value);
    void read_uint8(uint8_t& value);
    void read_uint32(uint32_t& value);
    void read_int8(int8_t& value);
    void read_rect(RECT& r);
    void read_fixed8(float& value);
    void read_fixed(float& value);

private:
    void reset_buffer(uint32_t size);

    void align_byte();
    ErrorCode parseTag();

private:
    std::vector<uint8_t> buffer_;
    bool parse_head_{false};
    CompressMode mode_{CompressMode::UNKNOWN};
    uint8_t version_{0};
    uint32_t datasize_;
    uint32_t totalpos_{0};
    uint32_t pos_{0};
    int bitpos_{0};
    ParseState state_{ParseState::SWF_NONE};
    RECT frameSize_;
    float frameRate_{0};
    uint16_t frameCount_{0};
    tag_parser tag_parser_;
    TagType tag_;
    uint32_t taglength_;
    head_complete_func head_complete_;
};
}  // namespace swf
