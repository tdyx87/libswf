// 对照逆向结果：SWF 位流层
//   sub_477E40 位流开始 / sub_477E50 位流结束
//   sub_4791E0(n) = ReadUB[n]（无符号）
//   sub_479240(n) = ReadSB[n]（有符号，符号扩展）
//   sub_478A21    = 位缓冲 refill（大端组装 4 字节，不足补 0，越界置 this+18）
//   sub_47A8A0    = RECT：NBits = UB[5]，随后 4 个 SB[NBits]
#pragma once

#include <cstdint>
#include "swf/ByteView.hpp"

namespace swf {

class BitReader {
public:
    explicit BitReader(ByteView& src) : src_(src) {}

    // sub_478A21：从字节流取 4 字节大端组装；可用不足则高位补 0
    std::uint32_t refill() {
        std::uint32_t result = 0;
        std::size_t avail = src_.remaining();
        for (int i = 0; i < 4; ++i) {
            result <<= 8;
            if (src_.has(1) && (avail == ByteView::kUnknownSize || i < static_cast<int>(avail))) {
                result |= src_.readUI8();
            }
        }
        return result;
    }

    // sub_4791E0：ReadUB[n]
    // 位缓冲 bitBuf_ 高位为下一个待读位；bitCount_ 为有效位数。
    std::uint32_t readUB(int n) {
        if (n <= 0) return 0;
        if (n > 32) n = 32;  // 防止 32 - n 为负导致双重移位 UB
        int cnt = bitCount_;
        std::uint32_t buf = bitBuf_;
        std::uint32_t result;
        if (cnt >= n) {
            result = buf >> (32 - n);
            bitBuf_ = (cnt == n) ? 0u : (buf << n);
            bitCount_ = cnt - n;
        } else {
            // 跨 refill：旧缓冲提供高位 cnt 位，新缓冲提供低位 (n-cnt) 位
            int takeOld = cnt;
            int takeNew = n - cnt;
            std::uint32_t high = (takeOld > 0) ? (buf >> (32 - takeOld)) : 0u;
            buf = refill();
            if (takeNew >= 32) {  // 整块新缓冲：避免 buf << 32 / high << 32 的 UB
                bitBuf_ = 0;
                bitCount_ = 0;
                return buf;
            }
            std::uint32_t low = (takeNew > 0) ? (buf >> (32 - takeNew)) : 0u;
            result = (high << takeNew) | low;
            bitBuf_ = (buf << takeNew);
            bitCount_ = 32 - takeNew;
        }
        return result;
    }

    // sub_479240：ReadSB[n] = ReadUB[n] 后符号扩展
    std::int32_t readSB(int n) {
        if (n <= 0) return 0;
        if (n > 32) n = 32;
        std::uint32_t raw = readUB(n);
        if (n == 32) return static_cast<std::int32_t>(raw);
        return static_cast<std::int32_t>(raw << (32 - n)) >> (32 - n);
    }

    // sub_477E50：位流结束 —— 把 refill 预读但未被消费的整字节退回给字节流
    void finish() {
        // 语义：把 refill 预读但未消费的**整字节**退回，落点即"向上对齐到下一字节"。
        // 等价推导：缓冲覆盖 [P-4, P)，已消费 c = 32 - bitCount_ 位，
        //   对齐位 = (P-4) + ceil(c/8) = P - floor(bitCount_/8)
        // 两种写法完全等价，这里保留更简洁的前者对应形式。
        // 注意：不能用 floor(c/8)——那会少退一整字节，SWF 头（frameRate/frameCount）
        // 会因此整体错位（实测会读出 frameCount=11794 这种垃圾值）。
        std::size_t back = static_cast<std::size_t>(bitCount_) / 8;
        std::size_t p = src_.pos();
        src_.seek(back <= p ? p - back : 0);
        bitBuf_ = 0;
        bitCount_ = 0;
    }

    // 把字节流位置**前进**到当前位位置之后的整字节边界，并清空位缓冲。
    // 用于位字段与字节结构混排的场合（例如 SHAPE 记录里的 FILLSTYLEARRAY）：
    // finish() 是"把预读的整字节退回"，语义相反，不能用于此处。
    void alignToByte() {
        if (bitCount_ <= 0) return;         // 无缓冲：字节位置本来就是对的
        // refill() 一次装入 4 字节，故 src_.pos() == P 时缓冲覆盖 [P-4, P)，
        // 已消费 c = 32 - bitCount_ 位，下一个位所在字节 = (P-4) + ceil(c/8)。
        int consumed = 32 - bitCount_;
        std::size_t p = src_.pos();
        std::size_t start = (p >= 4) ? p - 4 : 0;
        src_.seek(start + static_cast<std::size_t>((consumed + 7) / 8));
        bitBuf_ = 0;
        bitCount_ = 0;
    }

    bool overflow() const { return src_.overflow(); }

    // 逻辑位位置（相对字节流起点）：src_.pos()*8 减去尚未消费的缓冲位。
    std::size_t bitPos() const {
        return src_.pos() * 8 - static_cast<std::size_t>(bitCount_);
    }

private:
    ByteView& src_;
    std::uint32_t bitBuf_ = 0;   // this[5]
    int bitCount_ = 0;           // this[6]
};

struct Rect {
    std::int32_t xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    std::int32_t width() const { return xMax - xMin; }
    std::int32_t height() const { return yMax - yMin; }
};

inline Rect readRect(BitReader& br) {  // sub_47A8A0
    Rect r;
    std::uint32_t nBits = br.readUB(5);
    r.xMin = br.readSB(static_cast<int>(nBits));
    r.xMax = br.readSB(static_cast<int>(nBits));
    r.yMin = br.readSB(static_cast<int>(nBits));
    r.yMax = br.readSB(static_cast<int>(nBits));
    return r;
}

}  // namespace swf
