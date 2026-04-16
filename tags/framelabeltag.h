#pragma once
#include "controltag.h"
#include <string>
class FrameLabelTag : public ControlTag {
public:
    FrameLabelTag(uint32_t length);
    ~FrameLabelTag();
    void SetName(const std::string& name);
    std::string GetName() const;
    void SetNamedAnchorFlag(bool flag);
    bool GetNamedAnchorFlag() const;

private:
    std::string name_;
    bool named_anchor_flag_;
};