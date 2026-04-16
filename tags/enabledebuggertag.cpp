#include "enabledebuggertag.h"

EnableDebuggerTag::EnableDebuggerTag(uint16_t length)
    : ControlTag(TagType::TAG_ENABLEDEBUGGER, length)
{
}

EnableDebuggerTag::~EnableDebuggerTag() {}

void EnableDebuggerTag::SetPassword(const std::string& password)
{
    password_ = password;
}
