#pragma once
#include "tag.h"
class ControlTag : public Tag {
public:
    ControlTag(TagType tag, uint32_t length);
    ~ControlTag();

private:
};