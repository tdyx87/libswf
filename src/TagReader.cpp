#include "swf/TagReader.hpp"

namespace swf {

TagStatus readTagHeader(const std::uint8_t* data, std::size_t pos,
                        std::size_t avail, TagHeader& out) {
    if (!data || pos > avail || avail - pos < 2) return TagStatus::Incomplete;

    const std::uint16_t v = static_cast<std::uint16_t>(
        data[pos] | (static_cast<std::uint16_t>(data[pos + 1]) << 8));
    std::uint32_t len = v & 0x3Fu;
    std::size_t cursor = pos + 2;

    if (len == 63) {
        if (avail - cursor < 4) return TagStatus::Incomplete;
        len = static_cast<std::uint32_t>(data[cursor])
            | (static_cast<std::uint32_t>(data[cursor + 1]) << 8)
            | (static_cast<std::uint32_t>(data[cursor + 2]) << 16)
            | (static_cast<std::uint32_t>(data[cursor + 3]) << 24);
        cursor += 4;
    }

    // 减法比较，避免 cursor + len 回绕；同时拒绝超出剩余数据的长度字段。
    if (static_cast<std::size_t>(len) > avail - cursor) return TagStatus::Incomplete;

    out.code = static_cast<std::uint16_t>(v >> 6);
    out.length = len;
    out.dataOffset = cursor;
    out.dataEnd = cursor + len;
    return TagStatus::Ok;
}

}  // namespace swf
