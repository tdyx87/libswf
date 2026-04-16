#include "fileattributestag.h"

FileAttributesTag::FileAttributesTag(uint32_t length)
    : ControlTag(TagType::TAG_FILEATTRIBUTES, length)
{
}

FileAttributesTag::~FileAttributesTag() {}

void FileAttributesTag::SetUseDirectBlit(bool value)
{
    usedirectblit_ = value;
}

void FileAttributesTag::SetUseGPU(bool value)
{
    usegpu_ = value;
}

void FileAttributesTag::SetHasMetadata(bool value)
{
    hasmetadata_ = value;
}

void FileAttributesTag::SetActionScript3(bool value)
{
    actionscript3_ = value;
}

void FileAttributesTag::SetUseNetwork(bool value)
{
    usenetwork_ = value;
}
