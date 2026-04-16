#pragma once
#include "controltag.h"

class RemoveObjectTag : public ControlTag {
public:
    RemoveObjectTag(uint8_t length);
    ~RemoveObjectTag();

private:
    uint16_t characterid_;
    uint16_t depth_;
};