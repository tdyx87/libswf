#include "framelabeltag.h"

FrameLabelTag::FrameLabelTag(uint32_t length)
    : ControlTag(TagType::TAG_FRAMELABEL, length)
{
}

FrameLabelTag::~FrameLabelTag() {}

void FrameLabelTag::SetName(const std::string& name)
{
    name_ = name;
}

std::string FrameLabelTag::GetName() const
{
    return name_;
}

void FrameLabelTag::SetNamedAnchorFlag(bool flag)
{
    named_anchor_flag_ = flag;
}

bool FrameLabelTag::GetNamedAnchorFlag() const
{
    return named_anchor_flag_;
}
