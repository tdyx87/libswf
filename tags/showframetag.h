#pragma once
#include "controltag.h"
class ShowFrameTag : public ControlTag {
public:
    ShowFrameTag(uint32_t length);
    ~ShowFrameTag();
};