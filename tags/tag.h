#pragma once
#include "../tagdefine.h"

enum class TagFlag { TAG_CONTROL, TAG_DEFINE };

class Tag {
public:
    Tag(TagType tag, uint32_t length, TagFlag flag);
    ~Tag();
    TagType GetTagType() const
    {
        return tag_;
    }
    TagFlag GetTagFlag() const
    {
        return flag_;
    }

protected:
    TagType tag_;
    uint32_t length_;
    TagFlag flag_;  // tag属于什么类型
};