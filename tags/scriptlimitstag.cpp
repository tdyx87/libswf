#include "scriptlimitstag.h"

ScriptLimitsTag::ScriptLimitsTag(uint32_t length)
    : ControlTag(TagType::TAG_SCRIPTLIMITS, length)
{
}

ScriptLimitsTag::~ScriptLimitsTag() {}

void ScriptLimitsTag::SetMaxRecursionDepth(uint16_t value)
{
    max_recursion_depth_ = value;
}

void ScriptLimitsTag::SetScriptTimeoutSeconds(uint16_t value)
{
    script_timeout_seconds_ = value;
}
