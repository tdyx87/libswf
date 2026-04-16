#include "setbackgroundcolortag.h"

SetBackGroundColorTag::SetBackGroundColorTag(uint32_t length)
    : ControlTag(TagType::TAG_SETBACKGROUNDCOLOR, length)
{
}

SetBackGroundColorTag::~SetBackGroundColorTag() {}

void SetBackGroundColorTag::SetColor(uint8_t r, uint8_t g, uint8_t b)
{
    r_ = r;
    g_ = g;
    b_ = b;
}

void SetBackGroundColorTag::GetColor(uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = r_;
    g = g_;
    b = b_;
}
