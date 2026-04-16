#pragma once
#include "controltag.h"
#include <string>
#include <map>
class ImportAssetsTag : public ControlTag {
public:
    ImportAssetsTag(uint16_t length);
    ~ImportAssetsTag();
    void SetUrl(const std::string& url);
    void AddTag(uint16_t characterid, const std::string& name);

protected:
    std::string url_;
    std::map<uint16_t, std::string> tag_map_;
};