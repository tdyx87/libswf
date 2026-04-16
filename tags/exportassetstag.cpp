#include "exportassetstag.h"

ExportAssetsTag::ExportAssetsTag(uint32_t length)
    : ControlTag(TagType::TAG_EXPORTASSETS, length)
{
}

ExportAssetsTag::~ExportAssetsTag() {}

void ExportAssetsTag::AddTag(uint16_t charactorid, const std::string& name)
{
    tag_map_[charactorid] = name;
}
