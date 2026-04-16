#include "protecttag.h"

ProtectTag::ProtectTag(uint8_t length)
    : ControlTag(TagType::TAG_PROTECT, length)
{
}

ProtectTag::~ProtectTag() {}
