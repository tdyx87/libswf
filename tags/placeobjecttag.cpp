#include "placeobjecttag.h"

PlaceObjectTag::PlaceObjectTag(uint32_t length)
    : ControlTag(TagType::TAG_PLACEOBJECT, length)
{
}

PlaceObjectTag::~PlaceObjectTag() {}

void PlaceObjectTag::SetCharacterId(uint16_t value)
{
    characterid_ = value;
}

void PlaceObjectTag::SetDepth(uint16_t value)
{
    depth_ = value;
}

void PlaceObjectTag::SetMatrix(swf::MATRIX matrix)
{
    matrix_ = matrix;
}

void PlaceObjectTag::SetCXFORM(swf::CXFORM form)
{
    colortransform = form;
}
