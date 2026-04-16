#include "tag.h"
Tag::Tag(TagType tag, uint32_t length, TagFlag flag)
    : tag_(tag), length_(length), flag_(flag)
{
}

Tag::~Tag() {}