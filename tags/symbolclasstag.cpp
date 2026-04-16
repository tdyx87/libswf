#include "symbolclasstag.h"

SymbolClassTag::SymbolClassTag(uint32_t length)
    : ControlTag(TagType::TAG_SYMBOLCLASS, length)
{
}

SymbolClassTag::~SymbolClassTag() {}

void SymbolClassTag::AddTag(uint16_t charactorid, const std::string& name)
{
    tag_map_[charactorid] = name;
}
