#pragma once
#include "controltag.h"
class EndTag : public ControlTag {
public:
    EndTag(uint32_t length);
    ~EndTag();
};
