#pragma once
#include "tag.h"
class DefineTag : public Tag {
public:
    DefineTag(TagType tag, uint32_t length);
    ~DefineTag();

protected:
    uint32_t charactorid;
};