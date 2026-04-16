#pragma once
#include "controltag.h"

class SetBackGroundColorTag : public ControlTag {
public:
    SetBackGroundColorTag(uint32_t length);
    ~SetBackGroundColorTag();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
    void GetColor(uint8_t& r, uint8_t& g, uint8_t& b);

private:
    uint8_t r_;
    uint8_t g_;
    uint8_t b_;
};