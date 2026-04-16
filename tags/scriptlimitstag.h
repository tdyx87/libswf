#pragma once
#include "controltag.h"

class ScriptLimitsTag : public ControlTag {
public:
    ScriptLimitsTag(uint32_t length);
    ~ScriptLimitsTag();
    void SetMaxRecursionDepth(uint16_t value);
    void SetScriptTimeoutSeconds(uint16_t value);

private:
    uint16_t max_recursion_depth_;
    uint16_t script_timeout_seconds_;
};