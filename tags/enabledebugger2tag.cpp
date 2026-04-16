#include "enabledebugger2tag.h"

EnableDebugger2Tag::EnableDebugger2Tag(uint16_t length)
    : ControlTag(TagType::TAG_ENABLEDEBUGGER2, length)
{
}

EnableDebugger2Tag::~EnableDebugger2Tag() {}

void EnableDebugger2Tag::SetPassword(const std::string& password)
{
    password_ = password;
}
