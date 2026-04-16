#pragma once
#include "controltag.h"
#include "datatype/datatype.h"

class PlaceObjectTag : public ControlTag {
public:
    PlaceObjectTag(uint32_t length);
    ~PlaceObjectTag();
    void SetCharacterId(uint16_t value);
    void SetDepth(uint16_t value);
    void SetMatrix(swf::MATRIX matrix);
    void SetCXFORM(swf::CXFORM form);

private:
    uint16_t characterid_;
    uint16_t depth_;
    swf::MATRIX matrix_;
    swf::CXFORM colortransform;
};