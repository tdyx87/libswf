#include "importassetstag.h"

ImportAssetsTag::ImportAssetsTag(uint16_t length)
    : ControlTag(TagType::TAG_IMPORTASSETS, length)
{
}

ImportAssetsTag::~ImportAssetsTag() {}

void ImportAssetsTag::SetUrl(const std::string& url)
{
    url_ = url;
}

void ImportAssetsTag::AddTag(uint16_t characterid, const std::string& name)
{
    tag_map_[characterid] = name;
}
