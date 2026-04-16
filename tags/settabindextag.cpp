#include "settabindextag.h"

SetTabIndexTag::SetTabIndexTag(uint32_t length)
    : ControlTag(TagType::TAG_SETTABINDEX, length)
{
}

SetTabIndexTag::~SetTabIndexTag() {}

void SetTabIndexTag::SetTabIndex(uint16_t value)
{
    tabindex_ = value;
}