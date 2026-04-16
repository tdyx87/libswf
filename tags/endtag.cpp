#include "endtag.h"

EndTag::EndTag(uint32_t length) : ControlTag(TagType::TAG_END, length) {}

EndTag::~EndTag() {}
