#include "definetag.h"

DefineTag::DefineTag(TagType tag, uint32_t length)
    : Tag(tag, length, TagFlag::TAG_DEFINE)
{
}

DefineTag::~DefineTag() {}
