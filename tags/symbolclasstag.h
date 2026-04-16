#pragma once
#include "controltag.h"
#include <string>
#include <map>
class SymbolClassTag : public ControlTag {
public:
    SymbolClassTag(uint32_t length);
    ~SymbolClassTag();
    void AddTag(uint16_t charactorid, const std::string& name);

private:
    std::map<uint16_t, std::string> tag_map_;
};