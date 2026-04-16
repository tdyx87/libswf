#pragma once
#include "controltag.h"
#include "datatype/datatype.h"
#include "actions/actions.h"
#include "filter/filter.h"
class PlaceObject3Tag : public ControlTag {
public:
    PlaceObject3Tag(uint32_t length);
    ~PlaceObject3Tag();

private:
    int flag;
    uint16_t depth_;
    std::string class_name_;
    uint16_t characterid_;
    swf::MATRIX matrix_;
    swf::CXFORMWITHALPHA colortransform_;
    uint16_t ratio_;
    std::string name_;
    uint16_t clipdepth_;
    std::vector<swf::FILTER*> filter_list_;
    uint8_t blend_mode_;
    uint8_t bitmap_cache_;
    uint8_t visible_;
    swf::RGBA background_color_;
    swf::CLIPACTIONS clip_actions_;
};