// 对照逆向结果：SWF tag parser 的字节缓冲模型
//   sub_47A550 中 this[0]=缓冲基址, this[1]=读游标, this[2]=tag 起始(回滚点),
//   this[3]=tag 数据结束, *(BYTE*)(this+18)=越界标志
//   sub_423C7A = 读 UI8, sub_423CA8 = 读 UI16, sub_47A7A0 = 读 RGB/RGBA
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swf {

class ByteView {
public:
    static constexpr std::size_t kUnknownSize = static_cast<std::size_t>(-1);

    ByteView() = default;
    ByteView(const std::uint8_t* data, std::size_t size)
        : base_(data), size_(size), limit_(size) {}
    explicit ByteView(const std::vector<std::uint8_t>& buf)
        : base_(buf.data()), size_(buf.size()), limit_(buf.size()) {}

    const std::uint8_t* base() const { return base_; }
    std::size_t pos() const { return pos_; }
    std::size_t size() const { return size_; }
    bool overflow() const { return overflow_; }

    void seek(std::size_t p) { pos_ = p; }
    void mark() { saved_ = pos_; }
    void rewindToMark() { pos_ = saved_; }

    // 软边界：只收紧「可读范围」，不改变 size()/pos() 的绝对偏移语义。
    // 这样旧解析器里 bv.pos()、base + bv.pos()、doff + len 等表达式无需改动，
    // 但所有 readUI* 都会被自动限制在当前 tag 内。
    void setLimit(std::size_t end) { limit_ = end < size_ ? end : size_; }
    std::size_t limit() const { return limit_; }
    void noteOverflow() { overflow_ = true; }
    void clearOverflow() { overflow_ = false; }

    std::size_t remaining() const {
        if (size_ == kUnknownSize || limit_ == kUnknownSize) return kUnknownSize;
        return pos_ < limit_ ? limit_ - pos_ : 0;
    }

    bool has(std::size_t n) const {
        if (size_ == kUnknownSize || limit_ == kUnknownSize) return true;
        if (pos_ > limit_) return false;
        return n <= limit_ - pos_;  // 减法，避免 pos_ + n 回绕
    }

    std::uint8_t readUI8() {
        if (!has(1)) { overflow_ = true; return 0; }
        return base_[pos_++];
    }

    std::uint16_t readUI16() {  // 小端
        if (!has(2)) { overflow_ = true; return 0; }
        std::uint16_t v = static_cast<std::uint16_t>(base_[pos_]
                          | (base_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }

    std::uint32_t readUI32() {  // 小端
        if (!has(4)) { overflow_ = true; return 0; }
        std::uint32_t v = static_cast<std::uint32_t>(base_[pos_])
                        | (static_cast<std::uint32_t>(base_[pos_ + 1]) << 8)
                        | (static_cast<std::uint32_t>(base_[pos_ + 2]) << 16)
                        | (static_cast<std::uint32_t>(base_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }

    std::int32_t readSI32() { return static_cast<std::int32_t>(readUI32()); }

    struct RGBA {
        std::uint8_t r = 0, g = 0, b = 0, a = 0xFF;
    };

    RGBA readRGBA(bool withAlpha) {  // sub_47A7A0：不读 alpha 时填 -1
        RGBA c;
        c.r = readUI8();
        c.g = readUI8();
        c.b = readUI8();
        if (withAlpha) c.a = readUI8();
        else c.a = 0xFF;
        return c;
    }

    // pos_ 越过可读范围时返回 one-past-end 指针，避免构造出野指针。
    const std::uint8_t* current() const {
        const std::size_t p = (pos_ < limit_) ? pos_ : limit_;
        return base_ + p;
    }
    void skip(std::size_t n) {
        if (n > static_cast<std::size_t>(-1) - pos_) { pos_ = limit_; return; }
        pos_ += n;
    }

private:
    const std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t limit_ = 0;
    std::size_t pos_ = 0;
    std::size_t saved_ = 0;
    bool overflow_ = false;
};

}  // namespace swf
