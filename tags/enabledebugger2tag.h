#pragma once
#include "controltag.h"
#include <string>
class EnableDebugger2Tag : public ControlTag {
public:
    EnableDebugger2Tag(uint16_t length);
    ~EnableDebugger2Tag();
    void SetPassword(const std::string& password);

private:
    std::string password_;
};