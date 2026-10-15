// 对照逆向结果：
//   sub_490330 头解析 -> sub_48C230 标签循环 -> sub_48BA50 标签分派
//   返回码语义：0 成功 / 1 数据不足待续 / 2 遇 End / -1 数据不足(回滚) / -2 越界 / -5 中止
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swf/Format.hpp"
#include "swf/TagCode.hpp"

namespace swf {

struct TagInfo {
    std::uint16_t code = 0;
    std::uint32_t length = 0;
    std::size_t dataOffset = 0;
    std::string note;          // 浅解析得到的摘要
    std::uint16_t shapeId = 0; // DefineShape* 的 CharacterID
    Rect shapeBounds;
};

struct ParseResult {
    SwfHeader header;
    SwfBodyHeader body;
    std::vector<TagInfo> tags;
    std::size_t bodySize = 0;
    bool ok = false;
    std::string error;

    std::size_t frameTagCount() const;
    std::size_t definitionTagCount() const;
};

class Parser {
public:
    // 返回 sub_48C230 风格的状态码：0 成功 / 1 待续 / 2 End / 负数=错误
    int parse(const std::uint8_t* data, std::size_t size, ParseResult& out);
    int parseFile(const std::string& path, ParseResult& out);

private:
    int readTagHeader(const std::uint8_t* data, std::size_t pos, std::size_t avail,
                      TagHeader& th);
    void dispatch(const std::uint8_t* data, std::size_t avail, TagInfo& info);
};

}  // namespace swf
