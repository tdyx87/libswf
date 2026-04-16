#pragma once
#include "controltag.h"
#include "datatype/datatype.h"
class DefineScalingGridTag : public ControlTag {
public:
    DefineScalingGridTag(uint32_t length);
    ~DefineScalingGridTag();
    void SetCharacterId(uint16_t characterid);
    void SetSplitter(swf::RECT value);

private:
    uint16_t characterid_;
    swf::RECT splitter_;
};