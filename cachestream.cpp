#include "cachestream.h"
namespace swf {
CacheStream::CacheStream(const std::vector<uint8_t>& buffer)
    : buffer_(std::move(buffer))
{
}

CacheStream::CacheStream() {}

CacheStream::~CacheStream() {}

void CacheStream::Init(uint32_t capacity) {}

ErrorCode CacheStream::parse(const std::vector<uint8_t>& cache)
{
    buffer_.insert(buffer_.end(), cache.begin(), cache.end());
    ErrorCode code = ErrorCode::MOREDATA;
    do {
        switch (state_) {
            case ParseState::SWF_NONE: {
                if (buffer_.size() > 3) {
                    switch (buffer_[0]) {
                        case 'F':
                            mode_ = CompressMode::NO;
                            break;
                        case 'C':
                            mode_ = CompressMode::ZLIB;
                            break;
                        case 'Z':
                            mode_ = CompressMode::LAMA;
                            break;
                        default:
                            mode_ = CompressMode::UNKNOWN;
                    }
                } else {
                    break;
                }
                if (mode_ == CompressMode::UNKNOWN) {
                    code = ErrorCode::INVALID_SWF;
                    break;
                } else {
                    reset_buffer(3);
                    state_ = ParseState::SWF_SIGNATURE;
                }
            }
            case ParseState::SWF_SIGNATURE: {
                if (buffer_.size() > 0) {
                    version_ = buffer_[0];
                    reset_buffer(1);
                    state_ = ParseState::SWF_VERSION;
                } else {
                    break;
                }
            }
            case ParseState::SWF_VERSION: {
                if (buffer_.size() > 3) {
                    datasize_ =
                        *(reinterpret_cast<const uint32_t*>(buffer_.data()));
                    reset_buffer(4);
                    state_ = ParseState::SWF_FILELENGTH;
                } else {
                    break;
                }
            }
            case ParseState::SWF_FILELENGTH: {
                if (buffer_.size() > 9) {
                    read_rect(frameSize_);
                    state_ = ParseState::SWF_FRAMESIZE;
                } else {
                    break;
                }
            }
            case ParseState::SWF_FRAMESIZE: {
                if (buffer_.size() > 2) {
                    read_fixed8(frameRate_);
                    state_ = ParseState::SWF_FRAMERATE;
                } else {
                    break;
                }
            }
            case ParseState::SWF_FRAMERATE: {
                if (buffer_.size() > 2) {
                    read_uint16(frameCount_);
                    state_ = ParseState::SWF_FRAMECOUNT;
                } else {
                    break;
                }
            }
            case ParseState::SWF_FRAMECOUNT: {
                state_ = ParseState::SWF_TAG;
                if (head_complete_) {
                    head_complete_();
                }
            }
            case ParseState::SWF_TAG:
            case ParseState::SWF_TAG_HEADR:
            case ParseState::SWF_TAG_LONG_HEADER:
            case ParseState::SWF_TAG_CONTENT: {
                code = parseTag();
                if (tag_ == TagType::TAG_END) {
                    code = ErrorCode::SUCCESS;
                    break;
                }
            }
        }
    } while (false);
    return code;
}

bool CacheStream::has_parse_head()
{
    return parse_head_;
}
void CacheStream::reset_buffer(uint32_t size)
{
    if (size != 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + size);
        totalpos_ += size;
    }
}
void CacheStream::read_ub(uint32_t& value, uint32_t nbits)
{
    value = 0;
    if (nbits == 0) return;
    uint8_t cur_byte = buffer_[pos_];
    for (int i = 0; i < nbits; i++) {
        value += (((cur_byte << bitpos_) & (1 << 7)) >> 7) << (nbits - 1 - i);
        bitpos_++;
        if (bitpos_ >= 8) {
            pos_++;
            bitpos_ = 0;
            cur_byte = buffer_[pos_];
        }
    }
}
void CacheStream::read_sb(int32_t& value, uint32_t nbits)
{
    uint32_t uval = 0;
    read_ub(uval, nbits);
    int32_t val = (int32_t)(uval);
    int shift = 32 - nbits;
    value = (val << shift) >> shift;
}
void CacheStream::read_uint16(uint16_t& value)
{
    value = *(reinterpret_cast<const uint16_t*>(buffer_.data()));
    reset_buffer(2);
}
void CacheStream::read_uint8(uint8_t& value)
{
    value = buffer_[0];
    reset_buffer(1);
}
void CacheStream::read_uint32(uint32_t& value)
{
    value = *(reinterpret_cast<const uint32_t*>(buffer_.data()));
    reset_buffer(4);
}
void CacheStream::read_int8(int8_t& value)
{
    uint8_t u8value = buffer_[0];
    value = (u8value & 0x80);
    reset_buffer(1);
}
void CacheStream::read_rect(RECT& r)
{
    read_ub(r.nbits, 5);
    read_sb(r.xmin, r.nbits);
    read_sb(r.xmax, r.nbits);
    read_sb(r.ymin, r.nbits);
    read_sb(r.ymax, r.nbits);
    align_byte();
}
void CacheStream::read_fixed8(float& value)
{
    uint8_t h, l;
    read_uint8(l);
    read_uint8(h);
    value = h + l / 256.0f;
}
void CacheStream::read_fixed(float& value)
{
    uint16_t h, l;
    read_uint16(l);
    read_uint16(h);
    value = h + l / 65536.0f;
}
void CacheStream::align_byte()
{
    if (bitpos_ != 0) {
        bitpos_ = 0;
        pos_++;
    }
    reset_buffer(pos_);
}
ErrorCode CacheStream::parseTag()
{
    ErrorCode code = ErrorCode::SUCCESS;
    do {
        uint16_t tagcodeandlength;
        switch (state_) {
            case ParseState::SWF_TAG: {
                if (buffer_.size() >= 2) {
                    read_uint16(tagcodeandlength);
                    tag_ = static_cast<TagType>(tagcodeandlength >> 6);
                    taglength_ = tagcodeandlength & 0x3F;
                    state_ = ParseState::SWF_TAG_HEADR;
                } else {
                    code = ErrorCode::MOREDATA;
                    break;
                }
            }
            case ParseState::SWF_TAG_HEADR: {
                if (taglength_ == 0x3F) {
                    if (buffer_.size() >= 4) {
                        read_uint32(taglength_);
                        state_ = ParseState::SWF_TAG_LONG_HEADER;
                    } else {
                        code = ErrorCode::MOREDATA;
                        break;
                    }
                } else {
                    state_ = ParseState::SWF_TAG_CONTENT;
                }
            }
            case ParseState::SWF_TAG_LONG_HEADER: {
                state_ = ParseState::SWF_TAG_CONTENT;
            }
            case ParseState::SWF_TAG_CONTENT: {
                if (taglength_ <= buffer_.size()) {
                    if (tag_parser_) {
                        tag_parser_(buffer_, taglength_, tag_);
                    }
                    reset_buffer(taglength_);
                    state_ = ParseState::SWF_TAG;
                } else {
                    code = ErrorCode::MOREDATA;
                    break;
                }
            }
        }
        if (code == ErrorCode::MOREDATA) break;
        printf("%d\n", tag_);
    } while (buffer_.size() > 0);
    return code;
}
}  // namespace swf
