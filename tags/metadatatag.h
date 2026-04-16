#pragma once
#include "controltag.h"
#include <string>
class MetadataTag : public ControlTag {
public:
    MetadataTag(uint32_t length);
    ~MetadataTag();
    void SetMetadata(const std::string& value);

private:
    std::string metadata_;
};