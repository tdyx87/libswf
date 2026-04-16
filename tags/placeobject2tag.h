#pragma once
#include "controltag.h"
#include "datatype/datatype.h"
#include "actions/actions.h"

class PlaceObject2Tag : public ControlTag {
public:
    PlaceObject2Tag(uint32_t length);
    ~PlaceObject2Tag();

private:
    int flag;
    uint16_t depth_;
    uint16_t characterid_;
    swf::MATRIX matrix_;
    swf::CXFORMWITHALPHA colortransform_;
    uint16_t ratio_;
    std::string name_;
    uint16_t clipdepth_;
    swf::CLIPACTIONS clip_actions_;
};