#include "metadatatag.h"

MetadataTag::MetadataTag(uint32_t length)
    : ControlTag(TagType::TAG_METADATA, length)
{
}

MetadataTag::~MetadataTag() {}

void MetadataTag::SetMetadata(const std::string& value)
{
    metadata_ = value;
}
