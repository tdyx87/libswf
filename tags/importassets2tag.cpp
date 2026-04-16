#include "importassets2tag.h"

ImprtAssets2Tag::ImprtAssets2Tag(uint32_t length)
    : ControlTag(TagType::TAG_IMPORTASSETS2, length)
{
}

ImprtAssets2Tag::~ImprtAssets2Tag() {}

void ImprtAssets2Tag::SetUrl(const std::string& url)
{
    url_ = url;
}

void ImprtAssets2Tag::AddTag(uint16_t characterid, const std::string& name)
{
    tag_map_[characterid] = name;
}
