#pragma once
#include "controltag.h"
class SetTabIndexTag : public ControlTag {
public:
    SetTabIndexTag(uint32_t length);
    ~SetTabIndexTag();
    void SetTabIndex(uint16_t value);

private:
    uint16_t tabindex_;
};