#include "controltag.h"

ControlTag::ControlTag(TagType tag, uint32_t length)
    : Tag(tag, length, TagFlag::TAG_CONTROL)
{
}

ControlTag::~ControlTag() {}
