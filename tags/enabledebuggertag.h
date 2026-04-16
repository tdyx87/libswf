#pragma once
#include "controltag.h"
#include <string>
class EnableDebuggerTag : public ControlTag {
public:
    EnableDebuggerTag(uint16_t length);
    ~EnableDebuggerTag();
    void SetPassword(const std::string& password);

private:
    std::string password_;
};