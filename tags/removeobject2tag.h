#pragma once
#include "controltag.h"

class RemoveObject2Tag : public ControlTag {
public:
    RemoveObject2Tag(uint8_t length);
    ~RemoveObject2Tag();

private:
    uint16_t depth_;
};