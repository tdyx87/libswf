#pragma once
#include "controltag.h"
#include <map>
class ExportAssetsTag : public ControlTag {
public:
    ExportAssetsTag(uint32_t length);
    ~ExportAssetsTag();
    void AddTag(uint16_t charactorid, const std::string& name);

private:
    uint16_t count_;
    std::map<uint16_t, std::string> tag_map_;
};
