#include "definescalinggridtag.h"

DefineScalingGridTag::DefineScalingGridTag(uint32_t length)
    : ControlTag(TagType::TAG_DEFINESCALINGGRID, length)
{
}

DefineScalingGridTag::~DefineScalingGridTag() {}

void DefineScalingGridTag::SetCharacterId(uint16_t characterid)
{
    characterid_ = characterid;
}

void DefineScalingGridTag::SetSplitter(swf::RECT value)
{
    splitter_.xmin = value.xmin;
    splitter_.xmax = value.xmax;
    splitter_.ymin = value.ymin;
    splitter_.ymax = value.ymax;
}
